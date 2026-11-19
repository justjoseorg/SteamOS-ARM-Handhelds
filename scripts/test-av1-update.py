import io
import importlib.util
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

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


class FlatpakVideoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        path = ROOT / "steamos-overlay/usr/lib/steamos-arm/hub/hub.py"
        spec = importlib.util.spec_from_file_location("hub_video_test", path)
        self.hub = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.hub)
        self.hub.HOME = self.work / "home"
        self.hub.VA_SYSTEM = self.work / "va"
        self.hub.catalog = lambda: []
        self.ref = "io.github.streetpea.Chiaki4deck"

    def test_steam_shortcut_gets_persistent_driver_and_updates(self):
        self.hub.VA_SYSTEM.mkdir()
        source = self.hub.VA_SYSTEM / "msm_drv_video.so"
        dest = self.hub.HOME / ".var/app" / self.ref / self.hub.VA_DIR / source.name
        with mock.patch.object(self.hub.subprocess, "run") as run:
            run.return_value.stdout = self.ref + "\n"
            for content in (b"original driver", b"updated driver"):
                source.write_bytes(content)
                self.assertEqual(self.hub.video_accel_sync(), 0)
                self.assertEqual(dest.read_bytes(), content)
                run.assert_called_with(
                    ["flatpak", "override", "--user", self.ref,
                     f"--env=LIBVA_DRIVERS_PATH={dest.parent}",
                     "--env=LIBVA_DRIVER_NAME=msm"], timeout=60)
                self.assertFalse(dest.with_name(dest.name + ".new").exists())

    def test_missing_driver_leaves_overrides_unchanged(self):
        with mock.patch.object(self.hub.subprocess, "run") as run:
            run.return_value.stdout = self.ref + "\n"
            self.assertEqual(self.hub.video_accel_sync(), 0)
            self.assertEqual(run.call_count, 1)
            self.assertFalse(self.hub.HOME.exists())

    def test_release_patch_adds_chiaki_to_upstream_hub(self):
        dest = self.work / "usr/lib/steamos-arm/hub/hub.py"
        dest.parent.mkdir(parents=True)
        dest.write_text(
            "# Flatpaks not started by the hub that should still decode on the video core.\n"
            "# They get a persistent per-app override instead of launch options.\n"
            'VIDEO_ACCEL_EXTRA = {"org.mozilla.firefox": {}}\n\n\n'
            'FIREFOX_PREFS_MARK = "// steamos-arm: hardware video decoding"\n')
        patch = ROOT / "fork/patches/20-chiaki-video-accel.patch"
        result = subprocess.run(
            ["patch", "--batch", "-p1", "-i", str(patch)], cwd=self.work,
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        namespace = {}
        exec(compile(dest.read_text(), str(dest), "exec"), namespace)
        self.assertEqual(namespace["VIDEO_ACCEL_EXTRA"], self.hub.VIDEO_ACCEL_EXTRA)


if __name__ == "__main__":
    unittest.main()
