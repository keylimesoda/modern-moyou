#!/usr/bin/env python3
"""Unpack PlayStation 3 QRC (QRCC/QRCF) archives such as lines.qrc."""

import argparse
import hashlib
import os
import struct
import zlib
from pathlib import Path


def read_cstr(buf: bytes, offset: int) -> str:
    end = buf.index(b"\x00", offset)
    return buf[offset:end].decode("utf-8", errors="replace")


def unpack_qrc(qrc_path: Path, out_dir: Path, verbose: bool = False) -> list[tuple[str, bytes]]:
    raw_file = qrc_path.read_bytes()
    magic = raw_file[:4]
    if magic == b"QRCC":
        uncompressed_size = struct.unpack(">I", raw_file[4:8])[0]
        raw = zlib.decompress(raw_file[8:])
        if len(raw) != uncompressed_size:
            raise ValueError(f"Decompressed size mismatch: {len(raw)} != {uncompressed_size}")
    elif magic == b"QRCF":
        raw = raw_file
    else:
        raise ValueError(f"Unsupported magic {magic!r} in {qrc_path}")

    if raw[:4] != b"QRCF":
        raise ValueError(f"Expected QRCF payload, got {raw[:4]!r}")

    (
        version,
        tbl_off, tbl_sz,
        id_off, id_sz,
        str_off, str_sz,
        int_off, int_sz,
        flt_off, flt_sz,
        data_off, data_sz,
    ) = struct.unpack(">13I", raw[4:56])

    tbl = raw[tbl_off : tbl_off + tbl_sz]
    id_tbl = raw[id_off : id_off + id_sz]
    str_tbl = raw[str_off : str_off + str_sz]
    data_blob = raw[data_off : data_off + data_sz]

    extracted: list[tuple[str, bytes]] = []

    def walk_node(node_off: int):
        while node_off != 0xFFFFFFFF:
            name_off, num_attrs, parent, prev_sib, next_sib, first_child, last_child = struct.unpack_from(
                ">7I", tbl, node_off
            )
            tag = read_cstr(str_tbl, name_off)
            attrs = {}
            attr_off = node_off + 28
            for _ in range(num_attrs):
                a_name_off, a_type, a_val1, a_val2 = struct.unpack_from(">4I", tbl, attr_off)
                a_name = read_cstr(str_tbl, a_name_off)
                attrs[a_name] = (a_type, a_val1, a_val2)
                attr_off += 16

            if tag == "file" and "src" in attrs and "id" in attrs:
                _, blob_off, blob_sz = attrs["src"]
                _, id_entry_off, _ = attrs["id"]
                # id_tbl entry: 4-byte back-pointer to node_off, followed by NUL-terminated path
                rel_path = read_cstr(id_tbl, id_entry_off + 4)
                payload = data_blob[blob_off : blob_off + blob_sz]
                extracted.append((rel_path, payload))

                dest = out_dir / rel_path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(payload)
                if verbose:
                    sha = hashlib.sha256(payload).hexdigest()[:12]
                    print(f"  {rel_path:45s} {len(payload):7d} bytes  sha256:{sha}")

            if first_child != 0xFFFFFFFF:
                walk_node(first_child)
            node_off = next_sib

    walk_node(0)
    return extracted


def main():
    parser = argparse.ArgumentParser(description="Unpack PS3 QRC (QRCC/QRCF) archive")
    parser.add_argument("qrc", type=Path, help="Input .qrc file")
    parser.add_argument("out", type=Path, help="Output directory")
    parser.add_argument("-v", "--verbose", action="store_true", help="List every extracted file")
    args = parser.parse_args()

    files = unpack_qrc(args.qrc, args.out, verbose=args.verbose)
    print(f"Extracted {len(files)} files from {args.qrc} -> {args.out}")


if __name__ == "__main__":
    main()
