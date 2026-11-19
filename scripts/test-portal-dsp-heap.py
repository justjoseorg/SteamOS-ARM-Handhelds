import gzip
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fix = load("portal_dsp_heap", ROOT / "scripts/fix-portal-dsp-heap.py")
boot = load("ufs_bootimg", ROOT / "external-and-mods/ufs-install/ufs-bootimg.py")


def tree(model="AYN Odin 2 Portal", compatible=b"shared-dma-pool\0"):
    names = bytearray()
    tokens = bytearray()

    def word(value):
        tokens.extend(struct.pack(">I", value))

    def text(value):
        tokens.extend(value)
        tokens.extend(b"\0" * (-len(tokens) % 4))

    def begin(name):
        word(1)
        text(name.encode() + b"\0")

    def prop(name, value):
        offset = len(names)
        names.extend(name.encode() + b"\0")
        word(3)
        word(len(value))
        word(offset)
        text(value)

    begin("")
    prop("model", model.encode() + b"\0")
    begin("reserved-memory")
    begin("adsp-rpc-remote-heap")
    prop("compatible", compatible)
    prop("reusable", b"")
    prop("size", struct.pack(">2I", 0, 0xC00000))
    prop("alignment", struct.pack(">2I", 0, 0x400000))
    word(2)
    begin("other-reservation")
    prop("no-map", b"")
    word(2)
    word(2)
    word(2)
    word(9)
    offset = 56
    header = struct.pack(">10I", 0xD00DFEED, offset + len(tokens) + len(names),
                         offset, offset + len(tokens), 40, 17, 16, 0,
                         len(names), len(tokens))
    return header + bytes(16) + tokens + names


def image(trees):
    kernel = gzip.compress(b"test Linux payload", mtime=0) + b"".join(trees)
    ramdisk = b"test initramfs"
    header = boot.HDR.pack(boot.MAGIC, len(kernel), 0x8000, len(ramdisk), 0x1000000,
                           0, 0, 0x100, 2048, 0, 0, b"test",
                           b"root=PARTLABEL=STORAGE", bytes(32), b"extra")

    def pad(data):
        return data + bytes(-len(data) % 2048)

    parsed = boot.BootImg(pad(header) + pad(kernel) + pad(ramdisk))
    return parsed.build(parsed.cmdline)


class PortalHeapTests(unittest.TestCase):
    def test_patch_preserves_other_trees_and_boot_payloads(self):
        before_tree = tree("AYN Odin 2")
        portal = tree()
        after_tree = tree("AYN Odin 2 Mini")
        source = image([before_tree, portal, after_tree])
        before = boot.BootImg(source)
        result = fix.patch_kernel(source)
        after = boot.BootImg(result)
        offset = len(before.kernel) - len(before_tree + portal + after_tree)
        self.assertEqual(after.kernel[:offset], before.kernel[:offset])
        self.assertEqual(after.kernel[offset:offset + len(before_tree)], before_tree)
        self.assertTrue(after.kernel.endswith(after_tree))
        self.assertEqual(len(result), len(source))
        self.assertEqual(after.ramdisk, before.ramdisk)
        self.assertEqual(after.cmdline, before.cmdline)
        self.assertEqual(after.extra, before.extra)
        self.assertEqual(after.id, after.expected_id())
        fixed_tree = after.kernel[offset + len(before_tree):-len(after_tree)]
        expected = {key: value[0] for key, value in fix.properties(portal).items()}
        del expected[(fix.NODE, "compatible")]
        del expected[(fix.NODE, "reusable")]
        expected[(fix.NODE, "no-map")] = b""
        self.assertEqual({key: value[0] for key, value
                          in fix.properties(fixed_tree).items()}, expected)
        self.assertEqual(fix.patch_kernel(result), result)

    def test_missing_duplicate_or_unexpected_heap_fails_closed(self):
        for trees in ([tree("Other")], [tree(), tree()],
                      [tree(compatible=b"unexpected-pool\0")]):
            with self.subTest(trees=len(trees)):
                with self.assertRaises(ValueError):
                    fix.patch_kernel(image(trees))

    def test_corrupt_boot_or_tree_is_rejected(self):
        corrupt_id = bytearray(image([tree()]))
        corrupt_id[576] ^= 1
        malformed_tree = bytearray(tree())
        struct.pack_into(">I", malformed_tree, 4, len(malformed_tree) + 1)
        for data in (bytes(corrupt_id), image([malformed_tree])):
            with self.subTest(size=len(data)):
                with self.assertRaises(ValueError):
                    fix.patch_kernel(data)

    def test_hook_writes_corrected_kernel_and_preserves_input(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source, output = work / "KERNEL", work / "KERNEL.fork"
            original = image([tree()])
            source.write_bytes(original)
            env = {**os.environ, "SOC": "sm8550", "FORK_UPSTREAM_KERNEL": "1",
                   "BOOT_KERNEL": str(source), "FORK_KERNEL": str(output)}
            command = ["bash", str(ROOT / "fork/hooks/50-dsp-heap-sm8550.sh")]
            result = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(output.read_bytes(), fix.patch_kernel(original))
            second = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(output.read_bytes(), fix.patch_kernel(original))

    def test_other_socs_and_older_kernel_are_not_modified(self):
        for soc, upstream in (("sm8650", "1"), ("sm8550", "0")):
            with self.subTest(soc=soc, upstream=upstream):
                result = subprocess.run(
                    ["bash", str(ROOT / "fork/hooks/50-dsp-heap-sm8550.sh")],
                    env={**os.environ, "SOC": soc, "FORK_UPSTREAM_KERNEL": upstream},
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
