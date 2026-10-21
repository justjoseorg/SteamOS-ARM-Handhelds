import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / "fork/hooks/30-av1-sm8550.sh"


class AV1UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.rootfs = self.work / "rootfs"
        self.rootfs.mkdir()

    def archive(self, names):
        with tarfile.open(self.work / "vaapi.tar.gz", "w:gz") as archive:
            for name in names:
                data = ("test " + name).encode()
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))

    def run_hook(self, soc):
        return subprocess.run(
            ["bash", str(HOOK)],
            env={**os.environ, "SOC": soc, "ROOTFS": str(self.rootfs),
                 "FORK_ASSETS": str(self.work), "TMPDIR": str(self.work)},
            capture_output=True, text=True,
        )

    def test_installs_driver_and_flatpak_copy_on_sm8550(self):
        self.archive(["msm_drv_video.so", "libgbm.so.1"])
        result = self.run_hook("sm8550")
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in ("usr/lib/dri/msm_drv_video.so",
                     "usr/lib/steamos-arm/va/msm_drv_video.so",
                     "usr/lib/steamos-arm/va/libgbm.so.1"):
            self.assertEqual((self.rootfs / path).read_text(), "test " + Path(path).name)
        service = self.rootfs / "usr/lib/systemd/user/default.target.wants/steamos-arm-video-accel.service"
        self.assertTrue(service.is_symlink())
        self.assertTrue(service.resolve().is_file())
        self.assertFalse(list(self.work.glob("tmp.*")))

    def test_missing_component_fails_without_installing_partial_driver(self):
        self.archive(["msm_drv_video.so"])
        result = self.run_hook("sm8550")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(list(self.rootfs.iterdir()))
        self.assertFalse(list(self.work.glob("tmp.*")))

    def test_other_socs_are_unchanged(self):
        result = self.run_hook("sm8650")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(list(self.rootfs.iterdir()))


if __name__ == "__main__":
    unittest.main()
