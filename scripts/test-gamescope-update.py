import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / "fork/hooks/40-gamescope-sm8550.sh"
PROFILE = "scripts/00-gamescope/displays/ayn.odin2.portal.lua"
LAYER = "libVkLayer_FROG_gamescope_wsi_aarch64.so"
MANIFEST = "VkLayer_FROG_gamescope_wsi.aarch64.json"


class GamescopeUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.rootfs = self.work / "rootfs"
        self.rootfs.mkdir()

    def archive(self, files):
        with tarfile.open(self.work / "gamescope.tar.gz", "w:gz") as archive:
            directory = tarfile.TarInfo("scripts")
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            archive.addfile(directory)
            for name, data in files.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                entry.mode = 0o644
                archive.addfile(entry, io.BytesIO(data))

    def run_hook(self, soc):
        return subprocess.run(
            ["bash", str(HOOK)],
            env={**os.environ, "SOC": soc, "ROOTFS": str(self.rootfs),
                 "FORK_ASSETS": str(self.work), "TMPDIR": str(self.work)},
            capture_output=True, text=True,
        )

    def test_installs_binary_profile_and_session_together(self):
        files = {
            "gamescope": b"test compositor",
            PROFILE: (ROOT / "external-and-mods/gamescope" / PROFILE).read_bytes(),
            "scripts/00-gamescope/common/util.lua": b"test utility",
            LAYER: b"test WSI layer",
            MANIFEST: b"test WSI manifest",
        }
        self.archive(files)
        custom = self.rootfs / "usr/local/share/gamescope/scripts/custom.lua"
        custom.parent.mkdir(parents=True)
        custom.write_text("existing script")
        result = self.run_hook("sm8550")
        self.assertEqual(result.returncode, 0, result.stderr)
        binary = self.rootfs / "usr/local/bin/gamescope"
        self.assertEqual(binary.read_bytes(), files["gamescope"])
        self.assertEqual(binary.stat().st_mode & 0o777, 0o755)
        for path in (PROFILE, "scripts/00-gamescope/common/util.lua"):
            installed = self.rootfs / "usr/local/share/gamescope" / path
            self.assertEqual(installed.read_bytes(), files[path])
        session = self.rootfs / "usr/lib/steamos/gamescope-session"
        self.assertEqual(
            session.read_bytes(),
            (ROOT / "steamos-overlay/usr/lib/steamos/gamescope-session").read_bytes(),
        )
        self.assertEqual(session.stat().st_mode & 0o777, 0o755)
        self.assertEqual(custom.read_text(), "existing script")
        self.assertEqual((self.rootfs / "usr/lib" / LAYER).read_bytes(), files[LAYER])
        self.assertEqual((self.rootfs / "usr/share/vulkan/implicit_layer.d" / MANIFEST).read_bytes(),
                         files[MANIFEST])
        self.assertFalse(list(self.work.glob("tmp.*")))

    def test_incomplete_component_fails_before_installation(self):
        for files in (
            {"gamescope": b"test compositor"},
            {PROFILE: b"test profile"},
            {"gamescope": b"", PROFILE: b"test profile"},
            {"gamescope": b"test compositor", PROFILE: b""},
            {"gamescope": b"test compositor", PROFILE: b"test profile", LAYER: b"test layer"},
            {"gamescope": b"test compositor", PROFILE: b"test profile",
             LAYER: b"", MANIFEST: b"test manifest"},
        ):
            with self.subTest(files=files):
                self.archive(files)
                result = self.run_hook("sm8550")
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(result.stderr)
                self.assertFalse(list(self.rootfs.iterdir()))
                self.assertFalse(list(self.work.glob("tmp.*")))

    def test_missing_archive_fails(self):
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
