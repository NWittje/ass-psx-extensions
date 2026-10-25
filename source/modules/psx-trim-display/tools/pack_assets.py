"""Compile native-reference RGB565 data into read-only role-selected C++ assets; stdlib only."""
import argparse
import hashlib
import pathlib
import struct

parser = argparse.ArgumentParser()
parser.add_argument("input", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()
ticks = [int(v) for v in (args.input / "ticks-q6.txt").read_text().split()]
if len(ticks) != 31 or any(a >= b for a, b in zip(ticks, ticks[1:])):
    raise ValueError("Expected 31 increasing half-degree tick centres")
parts = ['#include "trim/instrument.hpp"\nnamespace trim::assets {\n']
parts.append("static_assert(layout::scale_ticks_q6 == std::array<std::uint16_t,31>{"
             + ",".join(map(str, ticks)) + "},\"Requalify native scale geometry\");\n")
regions = {"off": (151, 49, 17, 82), "bar": (151, 52, 17, 327),
           "green_down": (130, 93, 14, 40), "green_up": (130, 233, 14, 45)}
for suffix, bounds in regions.items():
    scene_name = "off_plate" if suffix == "off" else suffix
    parts.append(f"static_assert({scene_name} == Rect{{{','.join(map(str,bounds))}}});\n")


def crop(data, bounds):
    x, y, width, height = bounds
    if len(data) != 307200:
        raise ValueError("Native artwork dimensions changed")
    return b"".join(data[((y + row) * 320 + x) * 2:
                         ((y + row) * 320 + x + width) * 2] for row in range(height))


for role, name in [(1, "captain"), (2, "first_officer")]:
    face = (args.input / (name + ".rgb565")).read_bytes()
    if len(face) != 307200:
        raise ValueError("Native artwork dimensions changed")
    sprites = [(suffix, crop((args.input / (name + "_" + suffix + ".rgb565")).read_bytes(), bounds))
               for suffix, bounds in regions.items()]
    digest = hashlib.sha256(b"PXT-ART2\0" + face + b"".join(data for _, data in sprites)
                            + struct.pack("<31H", *ticks)).digest()
    parts.append(f"#if TRIM_BUILD_ROLE == 0 || TRIM_BUILD_ROLE == {role}\n")
    for suffix, data in [("face", face)] + sprites:
        pixels = struct.unpack("<" + "H" * (len(data) // 2), data)
        parts.append(f"static const std::uint16_t {name}_{suffix}[]={{\n")
        for start in range(0, len(pixels), 32):
            parts.append(",".join(str(v) for v in pixels[start:start + 32]) + ",\n")
        parts.append("};\n")
    parts.append(f"static constexpr std::array<std::uint8_t,32> {name}_hash={{{','.join(str(v) for v in digest)}}};\n#endif\n")
for method, value in [("background", "face[y*width+x]"), ("off_pixel", "off[(y-off_plate.y)*off_plate.w+x-off_plate.x]"),
                      ("bar_pixel", "bar[(y-bar.y)*bar.w+x-bar.x]"), ("identity", "hash")]:
    is_hash = method == "identity"
    parts.append(("std::array<std::uint8_t,32> " if is_hash else "std::uint16_t ") + method + "(Role r" + ("" if is_hash else ",int x,int y") + ") noexcept {\n")
    for role, name in [(1, "captain"), (2, "first_officer")]:
        parts.append(f"#if TRIM_BUILD_ROLE == 0 || TRIM_BUILD_ROLE == {role}\nif(r==Role::{name}) return {name}_{value};\n#endif\n")
    parts.append("return {};\n}\n")
parts.append("std::uint16_t green_pixel(Role r,bool up,int x,int y) noexcept {\n")
for role, name in [(1, "captain"), (2, "first_officer")]:
    parts.append(f"#if TRIM_BUILD_ROLE == 0 || TRIM_BUILD_ROLE == {role}\n")
    parts.append(f"if(r==Role::{name}) return up?{name}_green_up[(y-green_up.y)*green_up.w+x-green_up.x]:"
                 f"{name}_green_down[(y-green_down.y)*green_down.w+x-green_down.x];\n#endif\n")
parts.append("return {};\n}\n")
parts.append("}\n")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text("".join(parts), encoding="utf-8")
