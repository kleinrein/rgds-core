use crate::backend::RenderTargetMode;
use crate::buffer_pool::TexturePool;
use crate::descriptors::Descriptors;
use crate::filters::{FilterSource, VERTEX_BUFFERS_DESCRIPTION_FILTERS};
use crate::surface::target::CommandTarget;
use crate::utils::SampleCountMap;
use bytemuck::{Pod, Zeroable};
use std::sync::OnceLock;
use swf::BlurFilter as BlurFilterArgs;
use wgpu::util::StagingBelt;
use wgpu::{BufferSlice, CommandEncoder, RenderPipeline, TextureView};

/// This is a 1:1 match of `struct Filter` in `blur.wgsl`. See that, and the usage below, for more info.
/// Since WebGL requires 16 byte struct size (alignment), some of these fields (namely m2 and last_weight)
/// are passed in precomputed, even though they are trivial to get (addition/multiplication by constant).
/// The struct would have to be padded with dummy data otherwise anyway - these are at least useful.
#[repr(C)]
#[derive(Copy, Clone, Debug, Pod, Zeroable, PartialEq)]
struct BlurUniform {
    direction: [f32; 2],
    full_size: f32,
    m: f32,
    m2: f32,
    first_weight: f32,
    last_offset: f32,
    last_weight: f32,
}

pub struct BlurFilter {
    bind_group_layout: wgpu::BindGroupLayout,
    pipeline_layout: wgpu::PipelineLayout,
    pipelines: SampleCountMap<OnceLock<wgpu::RenderPipeline>>,
}

/// RG DS: how much smaller a blur this wide is worked out (BlurFilter::apply): the kernel left is at least 5 wide.
fn shrink_for(blur: f32) -> u32 {
    if blur >= 32.0 {
        4
    } else if blur >= 10.0 {
        2
    } else {
        1
    }
}

/// RGDS_FLASH_BLUR=full: every blur at full size.
fn full_size_blur() -> bool {
    static FULL: OnceLock<bool> = OnceLock::new();
    *FULL.get_or_init(|| std::env::var("RGDS_FLASH_BLUR").as_deref() == Ok("full"))
}

impl BlurFilter {
    pub fn new(device: &wgpu::Device) -> Self {
        let texture = wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                multisampled: false,
                sample_type: wgpu::TextureSampleType::Float { filterable: true },
                view_dimension: wgpu::TextureViewDimension::D2,
            },
            count: None,
        };
        let sampling = wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
            count: None,
        };
        let uniform_size = std::mem::size_of::<BlurUniform>() as u64;
        let bind_group_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            entries: &[
                texture,
                sampling,
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT | wgpu::ShaderStages::VERTEX,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: wgpu::BufferSize::new(uniform_size),
                    },
                    count: None,
                },
            ],
            label: create_debug_label!("Blur filter binds (with buffer)").as_deref(),
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: None,
            bind_group_layouts: &[Some(&bind_group_layout)],
            immediate_size: 0,
        });

        Self {
            pipelines: Default::default(),
            pipeline_layout,
            bind_group_layout,
        }
    }

    fn pipeline(&self, descriptors: &Descriptors, msaa_sample_count: u32) -> &wgpu::RenderPipeline {
        self.pipelines.get_or_init(msaa_sample_count, || {
            let label = create_debug_label!("Blur Filter ({} msaa)", msaa_sample_count);
            descriptors
                .device
                .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                    label: label.as_deref(),
                    layout: Some(&self.pipeline_layout),
                    vertex: wgpu::VertexState {
                        module: &descriptors.shaders.blur_filter,
                        entry_point: Some("main_vertex"),
                        buffers: &VERTEX_BUFFERS_DESCRIPTION_FILTERS,
                        compilation_options: Default::default(),
                    },
                    primitive: wgpu::PrimitiveState {
                        topology: wgpu::PrimitiveTopology::TriangleList,
                        strip_index_format: None,
                        front_face: wgpu::FrontFace::Ccw,
                        cull_mode: None,
                        polygon_mode: wgpu::PolygonMode::default(),
                        unclipped_depth: false,
                        conservative: false,
                    },
                    depth_stencil: None,
                    multisample: wgpu::MultisampleState {
                        count: msaa_sample_count,
                        mask: !0,
                        alpha_to_coverage_enabled: false,
                    },
                    fragment: Some(wgpu::FragmentState {
                        module: &descriptors.shaders.blur_filter,
                        entry_point: Some("main_fragment"),
                        targets: &[Some(wgpu::TextureFormat::Rgba8Unorm.into())],
                        compilation_options: Default::default(),
                    }),
                    multiview_mask: None,
                    cache: None,
                })
        })
    }

    pub fn apply(
        &self,
        descriptors: &Descriptors,
        texture_pool: &mut TexturePool,
        draw_encoder: &mut wgpu::CommandEncoder,
        _staging_belt: &mut StagingBelt,
        source: &FilterSource,
        filter: &BlurFilterArgs,
    ) -> Option<CommandTarget> {
        let sample_count = source.texture.sample_count();
        let format = source.texture.format();
        let pipeline = self.pipeline(descriptors, sample_count);

        // RG DS: a wide blur is worked out on a smaller picture and stretched back at the end. A box 20 pixels wide
        // over a picture half the size is a box 10 wide over a quarter of the pixels, and what it leaves has no detail
        // a stretch could lose; at full size three passes each way over a 640 x 420 backdrop kept the Mali-G52 busy
        // for some 50 ms a picture (Swords and Sandals). Each axis shrinks by its own blur only, so a blur along one
        // axis keeps the other sharp. RGDS_FLASH_BLUR=full blurs at full size, as Ruffle does.
        let (shrink_x, shrink_y) = if sample_count == 1 && !full_size_blur() {
            (shrink_for(filter.blur_x.to_f32()), shrink_for(filter.blur_y.to_f32()))
        } else {
            (1, 1)
        };
        let small = wgpu::Extent3d {
            width: source.size.0.div_ceil(shrink_x).max(1),
            height: source.size.1.div_ceil(shrink_y).max(1),
            depth_or_array_layers: 1,
        };
        let mut flip = CommandTarget::new(
            descriptors,
            texture_pool,
            small,
            format,
            sample_count,
            RenderTargetMode::FreshWithColor(wgpu::Color::TRANSPARENT),
            draw_encoder,
        );
        let mut flop = CommandTarget::new(
            descriptors,
            texture_pool,
            small,
            format,
            sample_count,
            RenderTargetMode::FreshWithColor(wgpu::Color::TRANSPARENT),
            draw_encoder,
        );

        let vertex_buffer =
            crate::filters::fresh_buffer(descriptors, bytemuck::cast_slice(&[source.vertices()]), wgpu::BufferUsages::VERTEX);

        let source_view = source.texture.create_view(&Default::default());
        let mut first = true;
        for _ in 0..(filter.num_passes() as usize) {
            for i in 0..2 {
                let horizontal = i % 2 == 0;
                let strength = if horizontal {
                    filter.blur_x.to_f32()
                } else {
                    filter.blur_y.to_f32()
                };
                // Full width of the kernel (left edge to right edge)
                let full_size = strength.min(255.0);
                if full_size <= 1.0 {
                    // A width of 1 or less is a noop (it'd just sample itself and nothing else)
                    continue;
                }
                // RG DS: in the pixels of the picture this pass reads: the source's own the first time, the
                // smaller picture's after that.
                let full_size = if first {
                    full_size
                } else {
                    full_size / if horizontal { shrink_x } else { shrink_y } as f32
                };

                let (previous_view, previous_vertices, previous_width, previous_height) = if first {
                    first = false;
                    (
                        &source_view,
                        vertex_buffer.slice(..),
                        source.texture.width() as f32,
                        source.texture.height() as f32,
                    )
                } else {
                    (
                        flip.color_view(),
                        descriptors
                            .quad
                            .vertex_buffer
                            .slice(descriptors.quad.vertices_filter.clone()),
                        flip.width() as f32,
                        flip.height() as f32,
                    )
                };

                // See this article for additional information on the fractional blur algorithm, as this
                // implementation was inspired by it: https://fgiesen.wordpress.com/2012/08/01/fast-blurs-2/

                // This is how much the blur "extends past" the center pixel to either side.
                let radius = (full_size - 1.0) / 2.0;

                // This is how many simple double-1 weighted pixel pairs we can sample in the center.
                // Note how we're not using floor() here. This is to guarantee that alpha is not 0 when
                // radius is a whole number: That would cause the division below to end the universe,
                // and more importantly, also waste at least one sampling of the texture (the first one).
                // This way, alpha is 1 instead in those cases (with m being one smaller), and the last
                // two samplings can be fused into one, at the right place and with the right weight.
                let m = radius.ceil() - 1.0;
                // Not the transparency kind. It's almost the fractional part of radius.
                // If radius is a whole number, however, it's 1 instead of 0.
                // The rounding is done to imitate the fixed-point calculations in Flash Player,
                // improving emulation accuracy somewhat.
                let alpha = ((radius - m) * 255.0).floor() / 255.0;

                // These control how and where the last pair of pixels are to be sampled,
                // so that the next-to-last will end up with an effective weight of 1.0,
                // and the last one with a weight of alpha. Note that the offset is relative
                // to the center of the next-to-last sampled pixel, in the range of 0 to 0.5.
                let last_offset = 1.0 / ((1.0 / alpha) + 1.0);
                let last_weight = alpha + 1.0;

                let uniform = BlurUniform {
                    direction: if horizontal {
                        [1.0 / previous_width, 0.0]
                    } else {
                        [0.0, 1.0 / previous_height]
                    },
                    full_size,
                    m,
                    m2: m * 2.0,
                    first_weight: alpha,
                    last_offset,
                    last_weight,
                };
                let uniform_buffer =
            crate::filters::fresh_buffer(descriptors, bytemuck::cast_slice(&[uniform]), wgpu::BufferUsages::UNIFORM);

                self.render_with_uniform_buffers(
                    descriptors,
                    draw_encoder,
                    pipeline,
                    &flop,
                    previous_view,
                    previous_vertices,
                    &uniform_buffer,
                );

                std::mem::swap(&mut flip, &mut flop);
            }
        }

        // RG DS: stretched back to the size asked for, one sample a pixel (the same shader with a kernel of one).
        if !first && (shrink_x, shrink_y) != (1, 1) {
            let full = CommandTarget::new(
                descriptors,
                texture_pool,
                wgpu::Extent3d {
                    width: source.size.0,
                    height: source.size.1,
                    depth_or_array_layers: 1,
                },
                format,
                sample_count,
                RenderTargetMode::FreshWithColor(wgpu::Color::TRANSPARENT),
                draw_encoder,
            );
            let uniform = BlurUniform {
                direction: [0.0, 0.0],
                full_size: 1.0,
                m: 0.0,
                m2: 0.0,
                first_weight: 0.0,
                last_offset: 0.0,
                last_weight: 1.0,
            };
            let uniform_buffer = crate::filters::fresh_buffer(
                descriptors,
                bytemuck::cast_slice(&[uniform]),
                wgpu::BufferUsages::UNIFORM,
            );
            self.render_with_uniform_buffers(
                descriptors,
                draw_encoder,
                pipeline,
                &full,
                flip.color_view(),
                descriptors
                    .quad
                    .vertex_buffer
                    .slice(descriptors.quad.vertices_filter.clone()),
                &uniform_buffer,
            );
            flip = full;
        }

        if first {
            // Nothing happened, don't return an empty unused texture
            None
        } else {
            Some(flip)
        }
    }

    fn render_with_uniform_buffers(
        &self,
        descriptors: &Descriptors,
        draw_encoder: &mut CommandEncoder,
        pipeline: &RenderPipeline,
        destination: &CommandTarget,
        source: &TextureView,
        vertices: BufferSlice,
        uniform_buffer: &wgpu::Buffer,
    ) {
        let filter_group = descriptors
            .device
            .create_bind_group(&wgpu::BindGroupDescriptor {
                label: create_debug_label!("Filter group").as_deref(),
                layout: &self.bind_group_layout,
                entries: &[
                    wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::TextureView(source),
                    },
                    wgpu::BindGroupEntry {
                        binding: 1,
                        resource: wgpu::BindingResource::Sampler(
                            descriptors.bitmap_samplers.get_sampler(false, true),
                        ),
                    },
                    wgpu::BindGroupEntry {
                        binding: 2,
                        resource: uniform_buffer.as_entire_binding(),
                    },
                ],
            });

        let mut render_pass = draw_encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: create_debug_label!("Blur filter").as_deref(),
            color_attachments: &[destination.color_attachments()],
            ..Default::default()
        });
        render_pass.set_pipeline(pipeline);

        render_pass.set_bind_group(0, &filter_group, &[]);

        render_pass.set_vertex_buffer(0, vertices);
        render_pass.set_index_buffer(
            descriptors.quad.index_buffer.slice(..),
            wgpu::IndexFormat::Uint32,
        );
        render_pass.draw_indexed(descriptors.quad.indices_quad.clone(), 0, 0..1);
    }
}
