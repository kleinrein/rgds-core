#!/bin/sh
set -e
W=$HOME/3ds-spike/dbg
cd $W/mesa-25.2.8
sudo rm -rf $W/azahar-none
[ -x $W/venv/bin/meson ] || { python3 -m venv --system-site-packages $W/venv >/dev/null 2>&1 || sudo apt-get install -y -q python3-venv >/dev/null; python3 -m venv --system-site-packages $W/venv; $W/venv/bin/pip -q install meson; }
$W/venv/bin/meson --version
python3 - <<'PY'
p='src/gallium/drivers/panfrost/pan_bo.c'; s=open(p).read()
def sub(a,b):
    global s
    assert s.count(a)==1,a
    s=s.replace(a,b)
if 'rgds spike' not in s:
    # keep the mapping of a cached BO
    sub("""      /* When the reference count goes to zero, we need to cleanup */
      panfrost_bo_munmap(bo);

""","""      /* rgds spike: cached BOs stay mapped, munmap/mmap per batch is expensive */

""")
    sub("""      if (!panfrost_bo_cache_put(bo))
         panfrost_bo_free(bo);""","""      if (!panfrost_bo_cache_put(bo)) {
         panfrost_bo_munmap(bo);
         panfrost_bo_free(bo);
      }""")
    # no madvise pair, no kernel labels per reuse
    sub("""      if (!pan_kmod_bo_make_unevictable(entry->kmod_bo)) {
         panfrost_bo_free(entry);
         continue;
      }
""","")
    sub("""   pan_kmod_bo_make_evictable(bo->kmod_bo);

""","")
    sub("""   /* Update the label to help debug BO cache memory usage issues */
   panfrost_bo_set_label(bo, "Unused (BO cache)");

""","")
    # the only other way out of the cache
    s=s.replace("""static void
panfrost_bo_cache_evict_stale_bos(""","""static void panfrost_bo_munmap(struct panfrost_bo *bo);

static void
panfrost_bo_cache_evict_stale_bos(""")
    a=s.index('panfrost_bo_cache_evict_stale_bos(struct panfrost_device *dev)')
    b=s.index('/* Tries to add a BO to the cache')
    s=s[:a]+s[a:b].replace('panfrost_bo_free(entry);','panfrost_bo_munmap(entry);\n      panfrost_bo_free(entry);')+s[b:]
    a=s.index('panfrost_bo_cache_evict_all(struct panfrost_device *dev)')
    b=s.index('int\npanfrost_bo_mmap')
    s=s[:a]+s[a:b].replace('panfrost_bo_free(entry);','panfrost_bo_munmap(entry);\n         panfrost_bo_free(entry);')+s[b:]
    open(p,'w').write(s)
print('patched')
# glthread packs every draw from a buffer without the index range glDrawRangeElements* gives it, and
# the driver thread then scans each draw's indices for the vertex range Panfrost's vertex job needs (a
# seventh of that thread in a Mario Kart 7 race). Ranged draws keep their range.
p='src/mesa/main/glthread_draw.c'; s=open(p).read()
if 'rgds spike: ranged' not in s:
    def sub(a,b):
        global s
        assert s.count(a)==1,a
        s=s.replace(a,b)
    sub('''static ALWAYS_INLINE void
draw_elements(GLuint drawid, GLenum mode, GLsizei count, GLenum type,''','''/* rgds spike: ranged draws keep their range (see draw_elements) */
struct marshal_cmd_DrawRangeElementsBaseVertex
{
   struct marshal_cmd_base cmd_base;
   GLenum8 mode;
   GLindextype type;
   GLsizei count;
   GLuint start;
   GLuint end;
   GLint basevertex;
   const GLvoid *indices;
};

static ALWAYS_INLINE void
draw_elements(GLuint drawid, GLenum mode, GLsizei count, GLenum type,''')
    sub('''         mode >= 32 || !((1u << mode) & ctx->SupportedPrimMask) /* GL_INVALID_ENUM */
         ))) {
      if (drawid == 0 && baseinstance == 0) {''','''         mode >= 32 || !((1u << mode) & ctx->SupportedPrimMask) /* GL_INVALID_ENUM */
         ))) {
      /* rgds spike: ranged draws keep their range. Upstream packs every VBO draw without
       * it, and the driver thread then scans the indices of each draw for the vertex
       * range the driver needs. The real entry point validates everything again, so this
       * is safe for the error cases too. */
      if (index_bounds_valid && drawid == 0 && baseinstance == 0 && instance_count == 1) {
         int cmd_size = sizeof(struct marshal_cmd_DrawRangeElementsBaseVertex);
         struct marshal_cmd_DrawRangeElementsBaseVertex *cmd =
               _mesa_glthread_allocate_command(ctx, DISPATCH_CMD_DrawRangeElementsBaseVertex, cmd_size);

         cmd->mode = MIN2(mode, 0xff); /* clamped to 0xff (invalid enum) */
         cmd->type = encode_index_type(type);
         cmd->count = count;
         cmd->start = min_index;
         cmd->end = max_index;
         cmd->basevertex = basevertex;
         cmd->indices = indices;
         return;
      }
      if (drawid == 0 && baseinstance == 0) {''')
    sub('''uint32_t
_mesa_unmarshal_DrawRangeElementsBaseVertex(struct gl_context *ctx,
                                            const struct marshal_cmd_DrawRangeElementsBaseVertex *cmd)
{
   unreachable("should never end up here");
   return 0;
}''','''uint32_t
_mesa_unmarshal_DrawRangeElementsBaseVertex(struct gl_context *ctx,
                                            const struct marshal_cmd_DrawRangeElementsBaseVertex *cmd)
{
   /* rgds spike: ranged draws keep their range */
   const GLenum mode = cmd->mode;
   const GLenum type = _mesa_decode_index_type(cmd->type);

   CALL_DrawRangeElementsBaseVertex(ctx->Dispatch.Current,
                                    (mode, cmd->start, cmd->end, cmd->count, type,
                                     cmd->indices, cmd->basevertex));
   return align(sizeof(*cmd), 8) / 8;
}''')
    open(p,'w').write(s)
p='src/gallium/drivers/panfrost/pan_screen.c'; s=open(p).read()
if 'rgds spike' not in s:
    i=s.index('   struct pipe_caps *caps = (struct pipe_caps *)&screen->base.caps;')
    j=s.index('\n}\n',i)
    # lets glthread start (mesa_glthread=true in run.sh)
    s=s[:j]+'\n\n   caps->map_unsynchronized_thread_safe = true; /* rgds spike */'+s[j:]
    open(p,'w').write(s)
PY
grep -n "panfrost_bo_set_label(bo, label)" src/gallium/drivers/panfrost/pan_bo.c || true
sed -i 's/^   panfrost_bo_set_label(bo, label);$/   bo->label = label; \/* rgds spike: no ioctl per reuse *\//' src/gallium/drivers/panfrost/pan_bo.c
rm -rf build
$W/venv/bin/meson setup build --prefix=/usr --libdir=lib -Dbuildtype=release -Db_ndebug=true \
  -Dc_args=-mcpu=cortex-a55 -Dcpp_args=-mcpu=cortex-a55 \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dglvnd=enabled \
  -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dllvm=enabled -Dshared-llvm=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled \
  -Dvideo-codecs= -Dtools= -Dbuild-tests=false -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dperfetto=false 2>&1 | tail -25
nice ninja -C build -j8 2>&1 | tail -5
DESTDIR=$W/mesa-inst nice ninja -C build install 2>&1 | tail -1
find $W/mesa-inst -name "*.so*" | head
df -h / | tail -1
