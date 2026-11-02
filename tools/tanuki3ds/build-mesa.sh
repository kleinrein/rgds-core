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
