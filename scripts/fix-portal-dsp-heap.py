#!/usr/bin/env python3
"""Keep the Portal's DSP-owned remote heap out of Linux's page allocator."""
import argparse
import importlib.util
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
NODE = "/reserved-memory/adsp-rpc-remote-heap"
MAGIC = b"\xd0\x0d\xfe\xed"


def properties(blob):
    header = struct.unpack_from(">10I", blob)
    magic, total, structs, strings, _, version, _, _, names_size, structs_size = header
    if (magic != 0xD00DFEED or total != len(blob) or version < 17
            or not 40 <= strings <= strings + names_size <= total
            or not 40 <= structs <= structs + structs_size <= total):
        raise ValueError("invalid device tree")
    names = blob[strings:strings + names_size]
    cursor, limit = structs, structs + structs_size
    stack, result = [], {}
    while cursor < limit:
        start = cursor
        token, = struct.unpack_from(">I", blob, cursor)
        cursor += 4
        if token == 1:
            end = blob.index(b"\0", cursor, limit)
            stack.append(blob[cursor:end].decode())
            cursor = (end + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced device tree")
            stack.pop()
        elif token == 3:
            size, nameoff = struct.unpack_from(">II", blob, cursor)
            cursor += 8
            if nameoff >= names_size or cursor + size > limit or not stack:
                raise ValueError("invalid device-tree property")
            name = names[nameoff:names.index(b"\0", nameoff)].decode()
            key = ("/" + "/".join(stack[1:]), name)
            if key in result:
                raise ValueError("duplicate device-tree property")
            result[key] = (blob[cursor:cursor + size], start, nameoff)
            cursor = (cursor + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError("unbalanced device tree")
            return result
        else:
            raise ValueError("unknown device-tree token")
    raise ValueError("device-tree end missing")


def patch_tree(blob):
    old = properties(blob)
    expected = {key: value[0] for key, value in old.items()}
    if (expected.get((NODE, "no-map")) == b"" and (NODE, "compatible") not in old
            and (NODE, "reusable") not in old):
        return blob
    if (expected.get((NODE, "compatible")) != b"shared-dma-pool\0"
            or expected.get((NODE, "reusable")) != b""
            or (NODE, "no-map") in old):
        raise ValueError("unexpected Portal DSP heap layout")
    nameoff = next((value[2] for key, value in old.items()
                    if key[1] == "no-map"), None)
    if nameoff is None:
        raise ValueError("device tree has no no-map property name")
    result = bytearray(blob)
    value, offset, _ = old[(NODE, "compatible")]
    size = 12 + ((len(value) + 3) & ~3)
    result[offset:offset + size] = struct.pack(">I", 4) * (size // 4)
    struct.pack_into(">I", result, old[(NODE, "reusable")][1] + 8, nameoff)
    del expected[(NODE, "compatible")]
    del expected[(NODE, "reusable")]
    expected[(NODE, "no-map")] = b""
    if {key: value[0] for key, value in properties(result).items()} != expected:
        raise ValueError("unrelated device-tree properties changed")
    return bytes(result)


def patch_kernel(data):
    spec = importlib.util.spec_from_file_location(
        "ufs_bootimg", ROOT / "external-and-mods/ufs-install/ufs-bootimg.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    image = module.BootImg(data)
    if image.id != image.expected_id() or image.build(image.cmdline) != data:
        raise ValueError("unexpected boot image checksum or layout")
    stream = zlib.decompressobj(31)
    stream.decompress(image.kernel)
    if not stream.eof:
        raise ValueError("truncated compressed kernel")
    tail = stream.unused_data
    compressed_size = len(image.kernel) - len(tail)
    modified = bytearray(image.kernel)
    cursor, found = 0, 0
    while cursor < len(tail):
        offset = tail.find(MAGIC, cursor)
        if offset < 0:
            break
        size, = struct.unpack_from(">I", tail, offset + 4)
        if not 40 <= size <= len(tail) - offset:
            raise ValueError("invalid appended device-tree size")
        tree = tail[offset:offset + size]
        props = properties(tree)
        if props.get(("/", "model"), (b"",))[0] == b"AYN Odin 2 Portal\0":
            fixed = patch_tree(tree)
            start = compressed_size + offset
            modified[start:start + size] = fixed
            found += 1
        cursor = offset + size
    if found != 1:
        raise ValueError(f"expected one Portal device tree, found {found}")
    image.kernel = bytes(modified)
    built = image.build(image.cmdline)
    checked = module.BootImg(built)
    if checked.id != checked.expected_id():
        raise ValueError("patched boot image checksum mismatch")
    return built


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    patched = patch_kernel(args.source.read_bytes())
    args.output.write_bytes(patched)
    print("Portal DSP heap reserved with no-map; other device trees unchanged")


if __name__ == "__main__":
    main()
