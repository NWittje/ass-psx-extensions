"""Shared native PE/PDB GUID and age inspection for maintained package tools.

Developer-only audit; no Python runtime is required by the native products.
Current publishers consume these functions without a historical build-directory dependency.
"""
import json
import math
import struct
import uuid
import hashlib
from pathlib import Path


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def pe_identity(path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    sections, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    assert magic in (0x10b, 0x20b)
    directories = optional + (112 if magic == 0x20b else 96)
    rva, size = struct.unpack_from("<II", data, directories + 6 * 8)
    for index in range(sections):
        section = optional + optional_size + index * 40
        virtual_size, address, raw_size, offset = struct.unpack_from("<IIII", data, section + 8)
        if address <= rva < address + max(virtual_size, raw_size):
            directory = offset + rva - address
            break
    else:
        raise ValueError("Missing PE debug directory")
    for entry in range(directory, directory + size, 28):
        kind, = struct.unpack_from("<I", data, entry + 12)
        offset, = struct.unpack_from("<I", data, entry + 24)
        if kind == 2 and data[offset:offset + 4] == b"RSDS":
            return str(uuid.UUID(bytes_le=data[offset + 4:offset + 20])), struct.unpack_from("<I", data, offset + 20)[0]
    raise ValueError("Missing RSDS identity")


def pdb_identity(path):
    data = Path(path).read_bytes()
    assert data.startswith(b"Microsoft C/C++ MSF 7.00")
    block, _, _, directory_size, _, block_map = struct.unpack_from("<6I", data, 32)
    count = math.ceil(directory_size / block)
    blocks = struct.unpack_from(f"<{count}I", data, block_map * block)
    directory = b"".join(data[number * block:(number + 1) * block] for number in blocks)[:directory_size]
    streams, = struct.unpack_from("<I", directory)
    sizes = struct.unpack_from(f"<{streams}I", directory, 4)
    cursor = 4 + streams * 4
    for index, size in enumerate(sizes):
        count = 0 if size == 0xffffffff else math.ceil(size / block)
        blocks = struct.unpack_from(f"<{count}I", directory, cursor)
        cursor += count * 4
        if index == 1:
            info = b"".join(data[number * block:(number + 1) * block] for number in blocks)[:size]
            return str(uuid.UUID(bytes_le=info[12:28])), struct.unpack_from("<I", info, 8)[0]
    raise ValueError("Missing PDB info stream")



if __name__ == "__main__":
    raise SystemExit("Use scripts/publish-display-network.ps1 for current package qualification; this module only supplies native symbol inspection.")
