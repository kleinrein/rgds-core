//! Where Ruffle draws: a texture of the core's own, and (for the software picture) a buffer it is copied into after
//! each drawing. Ruffle's own TextureTarget makes the buffer anew whenever the size is set, so the core keeps this
//! one: the copy can be left out (the picture handed to the player as a GL texture instead), and the texture made
//! with the usages that needs.

use ruffle_render_wgpu::target::{RenderTarget, RenderTargetFrame};
use ruffle_render_wgpu::utils::BufferDimensions;
use ruffle_render_wgpu::wgpu;

#[derive(Debug)]
pub struct FlashTarget {
    pub size: wgpu::Extent3d,
    pub texture: wgpu::Texture,
    /// The picture copied out after each drawing, for the software path; None when the player takes the texture.
    pub buffer: Option<(wgpu::Buffer, BufferDimensions)>,
    copy_back: bool,
}

#[derive(Debug)]
pub struct FlashFrame(wgpu::TextureView);

impl RenderTargetFrame for FlashFrame {
    fn into_view(self) -> wgpu::TextureView {
        self.0
    }
    fn view(&self) -> &wgpu::TextureView {
        &self.0
    }
}

const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

impl FlashTarget {
    pub fn new(device: &wgpu::Device, (width, height): (u32, u32), copy_back: bool) -> FlashTarget {
        let size = wgpu::Extent3d { width: width.max(1), height: height.max(1), depth_or_array_layers: 1 };
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("Flash picture"),
            size,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: FORMAT,
            view_formats: &[FORMAT],
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                | wgpu::TextureUsages::COPY_SRC
                | wgpu::TextureUsages::TEXTURE_BINDING,
        });
        let buffer = copy_back.then(|| {
            let dims = BufferDimensions::new(size.width as usize, size.height as usize, FORMAT);
            let buffer = device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("Flash picture read-back"),
                size: dims.padded_bytes_per_row as u64 * dims.height as u64,
                usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                mapped_at_creation: false,
            });
            (buffer, dims)
        });
        FlashTarget { size, texture, buffer, copy_back }
    }
}

impl RenderTarget for FlashTarget {
    type Frame = FlashFrame;

    fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        *self = FlashTarget::new(device, (width, height), self.copy_back);
    }
    fn format(&self) -> wgpu::TextureFormat {
        FORMAT
    }
    fn width(&self) -> u32 {
        self.size.width
    }
    fn height(&self) -> u32 {
        self.size.height
    }
    fn get_next_texture(&mut self) -> Option<FlashFrame> {
        Some(FlashFrame(self.texture.create_view(&Default::default())))
    }
    fn submit<I: IntoIterator<Item = wgpu::CommandBuffer>>(
        &self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        command_buffers: I,
        _frame: FlashFrame,
    ) -> wgpu::SubmissionIndex {
        let Some((buffer, dims)) = &self.buffer else { return queue.submit(command_buffers) };
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &self.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(dims.padded_bytes_per_row),
                    rows_per_image: None,
                },
            },
            self.size,
        );
        queue.submit(command_buffers.into_iter().chain(Some(encoder.finish())))
    }
}
