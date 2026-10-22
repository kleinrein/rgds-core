"""The checked-in DSCore fork tree is complete and carries no generated shader blobs."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build_rgds_vulkan_shaders as shaders

TREE = ROOT / "dscore"
SHADER_DIR = TREE / "src/core/gpu/vk/shaders"


class DscoreForkTests(unittest.TestCase):
    def test_hires_pipeline_sources_present(self):
        for name in (
            "src/core/gpu/rgds_compose_pool.h", "src/core/gpu/rgds_sample_plan.hpp", "src/core/gpu/vk/vk_lean.cpp",
            "src/frontend/sdl/rgds_stylus.hpp", "src/frontend/sdl/rgds_frontend_ui.hpp", "LICENSE", "FORK.md"
        ):
            self.assertTrue((TREE / name).is_file(), name)

    def test_generated_spirv_is_not_committed(self):
        self.assertEqual([n for n in shaders.SHADERS if (SHADER_DIR / n).exists()], [])
        for source in {shader for shader, _ in shaders.SHADERS.values()}:
            self.assertTrue((SHADER_DIR / source).is_file(), source)

    def test_retired_native_ui_is_gone(self):
        for name in ("menu.cpp", "menu.h", "settings.cpp", "settings.h"):
            self.assertFalse((TREE / "src/frontend/sdl" / name).exists(), name)


if __name__ == "__main__":
    unittest.main()
