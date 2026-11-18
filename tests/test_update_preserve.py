"""The offline updater keeps identity files and the sshd enablement links."""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("konkr_update", ROOT / "external-and-mods/konkr-update/konkr-update.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

SSHD = ("systemd/system/multi-user.target.wants/sshd.service", "systemd/system/sockets.target.wants/sshd.socket")


class Preserve(unittest.TestCase):
    def test_sshd_links_are_preserved_in_etc_and_overlay(self):
        for rel in SSHD:
            self.assertIn(rel, mod.PRESERVE)
            self.assertTrue(mod.preserved("root/etc/" + rel))
            self.assertTrue(mod.preserved("root/" + mod.UPPER + "/" + rel))

    def test_siblings_are_not_preserved(self):
        self.assertFalse(mod.preserved("root/etc/systemd/system/multi-user.target.wants/other.service"))
        self.assertFalse(mod.preserved("root/etc/systemd/system/multi-user.target.wants/sshd.service.bak"))
        self.assertFalse(mod.preserved("root/usr/" + SSHD[0]))

    def test_existing_entries_still_preserved(self):
        self.assertTrue(mod.preserved("root/etc/ssh/ssh_host_ed25519_key"))
        self.assertTrue(mod.preserved("root/etc/NetworkManager/system-connections/home.nmconnection"))


class BootHelperImport(unittest.TestCase):
    def test_retarget_keeps_archived_bytecode_unchanged(self):
        source = '''
class BootImg:
    def __init__(self, data):
        self.id = "valid"
        self.cmdline = "root=PARTUUID=source"
    def expected_id(self):
        return "valid"
    def build(self, cmdline):
        return cmdline.encode()
def initramfs_bytes(image):
    return b"konkr-update-recover"
def retarget(cmdline, root):
    return "root=" + root
'''
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            helper = root / "payload/ufs-bootimg.py"
            helper.parent.mkdir()
            helper.write_text(source)
            cache = Path(importlib.util.cache_from_source(str(helper)))
            cache.parent.mkdir()
            cache.write_bytes(b"archived bytecode from another interpreter")
            original = cache.read_bytes()
            kernel = root / "KERNEL"
            kernel.write_bytes(b"kernel")
            with patch.object(sys, "dont_write_bytecode", False):
                mod.retarget_kernel(kernel, root / "next-KERNEL", "PARTLABEL=STORAGE", helper)
                self.assertFalse(sys.dont_write_bytecode)
            self.assertEqual(cache.read_bytes(), original)
            self.assertEqual((root / "next-KERNEL").read_bytes(), b"root=PARTLABEL=STORAGE")

    def test_import_failure_restores_bytecode_setting(self):
        with tempfile.TemporaryDirectory() as temp:
            helper = Path(temp) / "ufs-bootimg.py"
            helper.write_text('raise RuntimeError("helper load failed")')
            with patch.object(sys, "dont_write_bytecode", False):
                with self.assertRaisesRegex(RuntimeError, "helper load failed"):
                    mod.retarget_kernel(Path(temp) / "KERNEL", Path(temp) / "next-KERNEL",
                                        "PARTLABEL=STORAGE", helper)
                self.assertFalse(sys.dont_write_bytecode)


if __name__ == "__main__":
    unittest.main()
