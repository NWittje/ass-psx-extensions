"""Create a source-pinned, configuration-free Release payload plan.

This is a packaging tool. It does not transfer files or launch product sessions.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import struct


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def pe_imports(path):
    data = path.read_bytes()
    u16 = lambda at: struct.unpack_from("<H", data, at)[0]
    u32 = lambda at: struct.unpack_from("<I", data, at)[0]
    nt = u32(0x3c)
    if data[nt:nt + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE: {path}")
    machine, count = struct.unpack_from("<HH", data, nt + 4)
    optional = nt + 24
    magic = u16(optional)
    if magic not in (0x10b, 0x20b):
        raise ValueError(f"Unsupported PE: {path}")
    directories = optional + (112 if magic == 0x20b else 96)
    sections = [struct.unpack_from("<4I", data, optional + u16(nt + 20) + 40 * i + 8) for i in range(count)]

    def offset(rva):
        for size, base, raw_size, raw in sections:
            if base <= rva < base + max(size, raw_size) and rva - base < raw_size:
                return raw + rva - base
        raise ValueError(f"Unmapped PE RVA: {path}")

    def string(rva):
        at = offset(rva)
        return data[at:data.index(0, at)].decode("ascii")

    result = []
    for index, width, name_offset in ((1, 20, 12), (13, 32, 4)):
        rva, _ = struct.unpack_from("<2I", data, directories + index * 8)
        if not rva:
            continue
        at = offset(rva)
        while any(data[at:at + width]):
            if index == 13 and not u32(at) & 1:
                raise ValueError("VA-based delay import is not qualified")
            result.append((string(u32(at + name_offset)), index == 13))
            at += width
    return machine, result


def symbol_functions(source):
    path = source / "modules/psx-cdu-client/tools/audit.py"
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    nodes = [n for n in tree.body if isinstance(n, (ast.Import, ast.ImportFrom))
             or isinstance(n, ast.FunctionDef) and n.name in ("pe_identity", "pdb_identity")]
    namespace = {}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), "exec"), namespace)
    return namespace


DEPENDENCIES = """Shared Windows dependencies

Extract each module archive into the same parent directory. Install the current
ASS Graphics runtime DLL/PDB there once. Applications first use an adjacent file;
only an absent adjacent file selects exactly one parent directory. An invalid
local override is reported, never silently replaced. CWD/PATH are not searched.

ASS Graphics: https://github.com/NWittje/ass-graphics-lib/releases
NORM source: https://github.com/USNavalResearchLaboratory/norm
B612 fonts: https://github.com/polarsys/b612

NORM and font binaries are deliberately absent from these downloads. Supply
Norm.dll, B612-Regular.ttf and B612-Bold.ttf beside the empty marker
place_norm_and_b612_here.txt. Use the upright Regular/Bold TTFs from B612.
NORM is used by the DU host, CDU client, coordinator, watchdog and X-Plane plugin. The
graphics DLL/B612 serve Windows graphical applications; the exporter and ESP32
firmware do not acquire those dependencies. The X-Plane plugin needs only NORM.
Use the PSX NORM maintenance recipe and pinned inputs described in
https://github.com/NWittje/ass-psx-extensions/blob/main/DEPENDENCIES.md
The upstream v1.5.9 source retains API version constants 1.5.7; the selected
maintenance DLL is identified by its recipe and Windows version 1.5.9.2.

For X-Plane, Norm.dll belongs in Resources/plugins/psx_xplane/ (parent of
win_x64/psx_xplane.xpl), or as an explicit override inside win_x64. It is not
resolved relative to X-Plane.exe. Preserve the aircraft/Resources overlay layout.

Downloads contain no saved operator settings, profiles, calibration, quicksaves
or customer files. Keep existing installation settings when updating. HWIO's
catalogue.xml/fds-modules.xml are factory operating definitions required at
startup; copy them only for a new installation. Never overwrite a cockpit's
existing calibrated or edited definitions with these factory copies.
"""


# Explicit product scope keeps a newly added bench application out of a public
# operational release until its intended role has been reviewed.
OPERATIONAL_PRODUCTS = frozenset((
    "psx-cdu-client", "psx-du-client", "psx-du-host", "psx-exporter",
    "psx-hwio", "psx-ios", "psx-trim-display", "psx-xp-coordinator",
    "psx-xpl-watchdog", "X-Plane",
))
BENCH_PRODUCTS = frozenset(("fds-at-test", "fds-trace", "psx-reposition-test", "psx-trim-test"))
OPERATIONAL_OMISSIONS = {
    "psx-hwio": frozenset(("hwio-fds-replay.exe", "hwio-fds-replay.pdb")),
    "psx-xp-coordinator": frozenset(("psx-observation-replay.exe", "psx-observation-replay.pdb")),
}


def make_plan(binary_root, sdk, source, work, profile="all"):
    if profile not in ("all", "operational"):
        raise ValueError("Unknown release preparation profile")
    if not binary_root.is_dir() or not sdk.is_dir() or work.exists():
        raise ValueError("Existing Release/SDK inputs and a fresh payload directory are required")
    work.mkdir(parents=True)
    symbols = symbol_functions(source)
    manifest_path = sdk / "producer-file-manifest.json"
    sdk_manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    graphics = sdk / "Release/bin/ass_graphics_lib.dll"
    graphics_pdb = sdk / "Release/symbols/ass_graphics_lib.pdb"
    if symbols["pe_identity"](graphics) != symbols["pdb_identity"](graphics_pdb):
        raise ValueError("Graphics DLL/PDB mismatch")
    # Match complete original font bytes, not family-name strings or glyph data.
    font_bytes = [(source / f"modules/psx-ios/vendor/fonts/{name}").read_bytes()
                  for name in ("B612-Regular.ttf", "B612-Bold.ttf")]
    if any(font in graphics.read_bytes() for font in font_bytes):
        raise ValueError("Graphics still contains an embedded B612 file")
    groups = []

    def row(path, relative):
        return dict(source=str(path), relative=relative, bytes=path.stat().st_size, sha256=sha(path))

    for directory in sorted(binary_root.iterdir()):
        if not directory.is_dir() or directory.name in ("psx-hwio-share", "licenses"):
            continue  # Same HWIO runtime; this local installation owns separate settings.
        product = directory.name
        if profile == "operational" and product not in OPERATIONAL_PRODUCTS:
            if product not in BENCH_PRODUCTS:
                raise ValueError(f"Unclassified operational release product: {product}")
            continue
        manifests = [p for p in directory.iterdir() if p.name.lower() in ("manifest.json", "package-manifest.json")]
        if len(manifests) != 1:
            raise ValueError(f"One qualified runtime manifest required: {product}")
        metadata = json.loads(manifests[0].read_text(encoding="utf-8-sig"))
        fields = {key.lower(): value for key, value in metadata.items()}
        declared = {}
        for item in fields.get("files", fields.get("payload", [])):
            entry = {key.lower(): value for key, value in item.items()}
            relative = next((entry[key] for key in ("relative", "file", "name", "path") if key in entry), None)
            if not isinstance(relative, str) or Path(relative).is_absolute() or ".." in Path(relative).parts:
                raise ValueError(f"Invalid manifest member: {product}")
            relative = relative.replace("\\", "/").lower()
            if relative in declared:
                raise ValueError(f"Duplicate manifest member: {product}/{relative}")
            declared[relative] = entry
        # Excluded operator/dependency files may legitimately be absent. Every
        # declared operating payload and legal notice must remain present.
        for relative in declared:
            leaf = Path(relative).name
            external_or_operator = leaf in ("norm.dll", "norm.pdb", "ass_graphics_lib.dll", "ass_graphics_lib.pdb")
            external_or_operator |= bool(re.search(r"(?i)\.(cfg|ini|bak|ttf|otf|woff2?|psxfmc)$", leaf))
            external_or_operator |= relative.startswith(("profiles/", "backups/")) or leaf.startswith("customer.")
            if not external_or_operator and not (directory / relative).is_file():
                raise ValueError(f"Missing declared operating payload: {product}/{relative}")
        payload, imports = [], []
        for file in sorted(directory.rglob("*")):
            if file.is_symlink():
                raise ValueError(f"Linked package member: {file}")
            if not file.is_file():
                continue
            relative = file.relative_to(directory).as_posix()
            lower = relative.lower()
            leaf = file.name.lower()
            if profile == "operational" and leaf in OPERATIONAL_OMISSIONS.get(product, ()):
                continue
            if leaf in ("manifest.json", "package-manifest.json", "readme.txt", "readme.md", "place_norm_and_b612_here.txt"):
                continue
            if leaf in ("norm.dll", "norm.pdb", "ass_graphics_lib.dll", "ass_graphics_lib.pdb"):
                continue
            if file.suffix.lower() in (".cfg", ".ini", ".bak", ".ttf", ".otf", ".woff", ".woff2", ".psxfmc") or lower.startswith("profiles/") or leaf.startswith("customer."):
                continue
            if re.search(r"(?i)\.(bak|backup)([.-].*)?$|\.cfg\..*$", leaf) or lower.startswith("backups/"):
                continue
            if product == "psx-hwio" and lower.startswith("defaults/"):
                continue
            factory_definition = product == "psx-hwio" and lower in ("catalogue.xml", "fds-modules.xml")
            if lower not in declared and not factory_definition:
                raise ValueError(f"Unlisted file in finished runtime: {product}/{relative}")
            origin = file
            if product == "psx-hwio" and leaf in ("catalogue.xml", "fds-modules.xml"):
                origin = source / "modules/psx-hwio/config" / leaf
            if product == "fds-trace" and lower in ("defaults/catalogue.xml", "defaults/fds-modules.xml"):
                origin = source / "modules/psx-hwio/config" / leaf
            if product == "psx-hwio" and lower.startswith("output-atlas/"):
                origin = source / "modules/psx-hwio/resources" / relative
            allowed = file.suffix.lower() in (".exe", ".dll", ".xpl", ".pdb")
            allowed |= lower.startswith(("licenses/", "corresponding-source/", "shaders/"))
            allowed |= product == "psx-hwio" and (leaf in ("catalogue.xml", "fds-modules.xml") or lower.startswith("output-atlas/"))
            allowed |= product == "fds-trace" and lower in ("defaults/catalogue.xml", "defaults/fds-modules.xml")
            allowed |= product == "psx-ios" and lower == "environment-time.bin"
            allowed |= product == "psx-exporter" and lower == "psx-exporter-agent.jar"
            allowed |= product == "X-Plane" and lower.startswith(("resources/plugins/psx_xplane/licenses/", "aircraft/747-400/", "support/sparky/"))
            allowed |= product == "psx-trim-display" and (lower.startswith(("captain/", "first-officer/")) or lower == "thirdpartynotices.txt")
            if not allowed or file.suffix.lower() in (".log", ".db", ".sqlite", ".sqlite3", ".pyc"):
                raise ValueError(f"Unclassified publication payload: {product}/{relative}")
            if lower in declared:
                entry = declared[lower]
                nested = entry.get("source", {})
                if not isinstance(nested, dict):
                    nested = {}
                expected_bytes = entry.get("bytes", entry.get("size", nested.get("bytes")))
                expected_sha = entry.get("sha256", nested.get("sha256"))
                if not isinstance(expected_sha, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected_sha):
                    raise ValueError(f"Invalid operating payload digest: {product}/{relative}")
                if file.stat().st_size != expected_bytes or sha(file) != expected_sha.upper():
                    raise ValueError(f"Qualified operating payload changed: {product}/{relative}")
            if file.suffix.lower() in (".exe", ".dll", ".xpl"):
                machine, dependencies = pe_imports(file)
                if any(font in file.read_bytes() for font in font_bytes):
                    raise ValueError(f"Embedded B612 in {file}")
                if any(name.lower() == "ass_graphics_lib.dll" and not delayed for name, delayed in dependencies):
                    raise ValueError(f"Graphics import cannot use parent folder: {file}")
                imports.append(dict(path=relative, machine=machine, imports=[name for name, _ in dependencies]))
                candidates = list(directory.rglob(file.stem + ".pdb"))
                own = file.stem.startswith(("psx-", "psx_", "fds-", "hwio-"))
                if (own or candidates) and machine in (0x14c, 0x8664) and not re.match(r"(?i)(msvcp|vcruntime)", leaf):
                    if len(candidates) != 1 or symbols["pe_identity"](file) != symbols["pdb_identity"](candidates[0]):
                        raise ValueError(f"Native symbol mismatch: {file}")
            payload.append(row(origin, relative))
        # Trace from operating images, including owned dynamically loaded capture
        # agents. An unused CRT must not keep its own transitive CRTs alive.
        def crt(path):
            return re.match(r"(?i)^(msvcp|vcruntime|concrt|ucrtbase).*\.dll$", Path(path).name)

        images = {item["path"].lower(): item for item in imports}
        reachable = {path for path in images if not crt(path)}
        pending = list(reachable)
        while pending:
            current = images[pending.pop()]
            folder = Path(current["path"]).parent.as_posix()
            for name in current["imports"]:
                for candidate in ((folder + "/" + name).lower(), name.lower()):
                    if candidate in images:
                        if candidate not in reachable:
                            reachable.add(candidate)
                            pending.append(candidate)
                        break
        payload = [item for item in payload if not crt(item["relative"]) or item["relative"].lower() in reachable]
        imports = [item for item in imports if item["path"].lower() in reachable]
        available = {item["relative"].lower(): item for item in payload}
        for item in imports:
            folder = str(Path(item["path"]).parent).replace("\\", "/")
            for name in item["imports"]:
                if name.lower() == "ass_graphics_lib.dll":
                    continue
                if product == "X-Plane" and Path(item["path"]).suffix.lower() == ".xpl" and item["machine"] == 0x8664 and name.lower() in ("xplm_64.dll", "xpwidgets_64.dll"):
                    continue  # Supplied by the simulator; SDK/host DLLs are not addon payloads.
                candidates = [name.lower(), (folder + "/" + name).lower()]
                system = Path("C:/Windows/SysWOW64" if item["machine"] == 0x14c else "C:/Windows/System32")
                if not any(path in available for path in candidates) and not (system / name).is_file() and not name.lower().startswith(("api-ms-win-", "ext-ms-win-")):
                    raise ValueError(f"Missing publication import: {product}/{item['path']} -> {name}")
        guide = work / f"{product}-README.txt"
        old_guides = [p for p in directory.iterdir() if p.name.lower() in ("readme.txt", "readme.md")]
        if len(old_guides) != 1:
            raise ValueError(f"One operating guide required: {product}")
        original = old_guides[0].read_text(encoding="utf-8-sig")
        paragraphs = re.split(r"\r?\n\s*\r?\n", original)
        if profile == "operational":
            operating = []
            for paragraph in paragraphs:
                if "psx-observation-replay.exe" in paragraph:
                    # This helper is called by the application's diagnostics
                    # UI; it is an operating dependency, unlike offline replay.
                    paragraph = ("psx-diagnostics-setup.exe is the coordinator's explicit Windows "
                                 "crash-dump policy helper. Normal coordinator instances use one stable "
                                 "Windows-session identity; existing sessions are never terminated.")
                if any(name in paragraph for name in OPERATIONAL_OMISSIONS.get(product, ())):
                    continue
                operating.append(paragraph)
            paragraphs = operating
        operation = "\n\n".join(p for p in paragraphs if not re.search(r"(?i)ass[ -]?graphics|B612|Norm\.dll|current package:", p))
        guide.write_text(f"{product} — Release\n\n{DEPENDENCIES}\nModule operation\n\n{operation}\n", encoding="utf-8")
        payload.append(row(guide, "README.txt"))
        if product == "X-Plane":
            marker = work / "place_norm_and_b612_here.txt"
            marker.touch(exist_ok=True)
            payload.append(row(marker, "Resources/plugins/psx_xplane/place_norm_and_b612_here.txt"))
        uses_graphics = any(name.lower() == "ass_graphics_lib.dll" for item in imports for name in item["imports"])
        external = (["ass_graphics_lib.dll", "B612-Regular.ttf", "B612-Bold.ttf"] if uses_graphics else [])
        if product in ("psx-du-host", "psx-cdu-client", "psx-xp-coordinator", "psx-xpl-watchdog", "X-Plane"):
            external.append("Norm.dll")
        version = None
        if product == "psx-trim-display":
            version = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))["version"]
        groups.append(dict(product=product, files=payload, imports=imports,
                           graphics=sdk_manifest["version"] if uses_graphics else None,
                           externalDependencies=external, version=version,
                           platform="esp32" if product == "psx-trim-display" else "windows-x64"))
    marker = work / "place_norm_and_b612_here.txt"
    marker.touch(exist_ok=True)
    guide = work / "README.txt"
    guide.write_text(DEPENDENCIES, encoding="utf-8")
    shared = [row(graphics, "ass_graphics_lib.dll"), row(graphics_pdb, "ass_graphics_lib.pdb"), row(marker, marker.name), row(guide, guide.name)]
    for notice in sorted((sdk / "Release/licenses").rglob("*")):
        if notice.is_file():
            shared.append(row(notice, "licenses/ass-graphics/" + notice.relative_to(sdk / "Release/licenses").as_posix()))
    groups.append(dict(product="ass-graphics-lib", files=shared, imports=[],
                       graphics=sdk_manifest["version"], version=sdk_manifest["version"],
                       externalDependencies=["B612-Regular.ttf", "B612-Bold.ttf"], platform="windows-x64"))
    if profile == "operational" and {g["product"] for g in groups} != OPERATIONAL_PRODUCTS | {"ass-graphics-lib"}:
        raise ValueError("Operational release is missing a current product")
    return dict(schema=1, configuration="Release", profile=profile, graphics=sdk_manifest["version"],
                producerManifestSHA256=sha(manifest_path), groups=groups,
                limits="Static PE/PDB/data closure; NORM/fonts supplied separately; no simulator or physical acceptance.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary-root", "sdk", "source", "work", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--profile", choices=("all", "operational"), default="all")
    args = parser.parse_args()
    result = make_plan(args.binary_root, args.sdk, args.source, args.work, args.profile)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
