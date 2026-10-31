# Omitted dependencies and data locations

Paths are relative to this repository unless a build variable is named. This
initial source import contains our application/shared code, first-party build
helpers and project icons. ASS Graphics remains a separate project. Vendor code,
compiled files, fonts, datasets, catalogues, tests, standalone diagnostic tools
and engineering documentation are intentionally absent.

| Omitted input | Intended position |
|---|---|
| ASS Graphics SDK | ASS Graphics1.4.5/ABI1 is external. Native project qualification uses the stable local paired SDK `P:/development/ass_graphics_lib/sdk/internal/1.4.5`, selected by `ASS_GRAPHICS_SDK_ROOT` / `AssGraphics_DIR`; its `producer-file-manifest.json` binds both configurations and original native-symbol authority. Public SDK/runtime downloads at https://github.com/NWittje/ass-graphics-lib/releases contain Release only. No graphics implementation, SDK or compiled graphics payload is included in this source repository. |
| X-Plane SDK 4.3 | `source/external/xplane-sdk-4.3/`, including `CHeaders/XPLM` and `Libraries/Win/XPLM_64.lib`, or the explicit `XPLANE_SDK_ROOT`. |
| LZMA SDK 26.05 | `source/external/lzma-sdk-26.05/`: qualified decoder C sources/headers and the matching `SHA256SUMS` expected by the plugin build. |
| DU/CDU NORM supplier inputs | `source/shared/norm/vendor/norm/include/` supplies the C API headers. Legacy private runtime inputs under `source/shared/norm/vendor/norm/runtime/` remain separate qualification inputs. The shared default is the qualified source-built PSX maintenance runtime below; neither edition is distributed with application downloads. |
| NORM source inputs and shared runtime | Obtain https://github.com/USNavalResearchLaboratory/norm tag v1.5.9 and https://github.com/USNavalResearchLaboratory/protolib commit702e909945446275f0313846e988612883fca94a. Place the matching `norm-v1.5.9.tar.gz` and `protolib-702e9099.tar.gz` under `source/shared/sync-network/vendor/`. Our included CMake recipe verifies pinned archive/patch identities and builds PSX maintenance2 (Windows version1.5.9.2, upstream API constants still1.5.7). See the standalone recipe below. |
| CivetWeb | `source/modules/psx-ios/vendor/civetweb/include/` and `source/modules/psx-ios/vendor/civetweb/src/civetweb.c`, with the supplier's notices. |
| B612 font files | Obtain upright proportional `B612-Regular.ttf` and `B612-Bold.ttf` from https://github.com/polarsys/b612/tree/master/fonts/ttf. Supply them externally in the shared runtime parent or adjacent application override. Both native graphics and IOS HTTP font assets load external files; neither EXE/DLL embeds them. Set `ASS_B612_ROOT` or CMake `PSX_RUNTIME_FONT_ROOT` to a local font folder for native build/qualification placement; these files never enter publication archives. Compatible B612 revisions are independently replaceable, without a runtime checksum/revision gate. |
| IOS timezone data | `source/modules/psx-ios/vendor/time/environment-time.bin`. The included `tools/build_time_data.py` accepts separately supplied timezone-boundary and tzdata inputs. |
| IOS aircraft artwork inputs | `source/external/744_sideview_facing_right.svg`, `738_sideview_facing_left.svg`, `738_sideview_facing_right.svg` and `747-400-top-front.svg`. |
| HWIO runtime definitions | `source/modules/psx-hwio/config/` for catalogue/profile source data; `source/modules/psx-hwio/resources/output-atlas/` for the separate image/layout data. Operator configuration, calibration and learned values belong to the installation and are not repository inputs. |
| Authored catalogue tables | `source/shared/psx-instructor/src/failure_catalogue.inc` and `source/modules/psx-hwio/src/legacy_catalogue_ids.inc`; the data tables are excluded from this source-only import. |
| CDU glyph data | `source/shared/cdu/resources/glyph-paths.tsv` and `glyph-profile.json`. |
| Third-party CRT shader inputs and derived outputs | `source/shared/cdu/shaders/crt-royale/` and the input `shaders/manifest.json`; generated `registry.hpp`, registry metadata, compiled shaders and lookup textures belong in `source/shared/cdu/shaders/generated/`. First-party conversion/registry scripts are included; supplier shader implementations, textures and corresponding licence/source payloads are not. |
| ESP32 firmware toolchain/components | The separately installed ESP-IDF selected by `IDF_PATH`; component versions are declared in `firmware/main/idf_component.yml`. `TRIM_ASSET_CPP` selects separately generated artwork C++ (normally the omitted `source/modules/psx-trim-display/assets/compiled_faces.cpp`). Native reference images, RGB565 data and fonts remain external. |
| HWIO manufacturer SDK inputs | Supply the qualified Phidget headers to the selected build and the installed FDS/PoKeys/Flight Illusion libraries required by the chosen adapters. Vendor DLLs and device data stay at their configured installation locations. |
| PSX installation and preserved exporter component | The licensed PSX JAR remains outside this repository and is selected by the preparation/build arguments. `source/external/exporter-fmc-capture-0.19.0.jar` is the omitted, separately qualified capture-only component used by the current exporter composition. No vendor JAR/class files or predecessor binary is included. |
| X-Plane aircraft/audio inputs | Original ACF/SND files, meshes, textures and sound banks remain in the external X-Plane aircraft installation. First-party preparation scripts are under `source/modules/xplane-plugin/scripts/`; their generated `assets/sparky_audio_manifest.inc` and `sparky-audio-manifest.json` remain omitted local inputs. |

The retained source/build files preserve the engineering source edition. They
can reference intentionally omitted qualification trees, local workflow helpers
and input metadata. This import is a source inventory, not a self-contained or
build-qualified distribution. No executable or package is published here yet.

The current first-party Windows build/publication scripts also use the maintained scripts/work-cleanup.ps1, scripts/package-layout.ps1 and scripts/deployment/ helpers in the full local project. Qualification harnesses, diagnostic/testing tools and their fixtures remain omitted; these source files do not make a standalone build distribution.

## Shared runtime placement

Extract application archives into one shared parent. The graphics runtime archive
places `ass_graphics_lib.dll` and its matching PDB in that parent once. Supply
`Norm.dll`, `B612-Regular.ttf` and `B612-Bold.ttf` beside the exact empty
`place_norm_and_b612_here.txt` marker. NORM/font binaries are absent from downloads.

```text
ASS-PSX/
  ass_graphics_lib.dll
  ass_graphics_lib.pdb
  place_norm_and_b612_here.txt
  Norm.dll                 # supplied separately
  B612-Regular.ttf         # supplied separately
  B612-Bold.ttf            # supplied separately
  psx-ios/psx-ios.exe
  psx-xp-coordinator/psx-xp-coordinator.exe
  psx-hwio/psx-hwio.exe
```

An adjacent file overrides the shared copy. Only absence selects exactly one
parent; an unreadable or invalid selected file reports its own failure. CWD,
PATH and higher ancestors are not lookup sources. NORM's explicit absolute-path
option remains available. A legacy NORM override may also need its matching CRT
DLLs alongside that override. The selected maintenance build uses a static CRT.

X-Plane resolves NORM from `Resources/plugins/psx_xplane/`, one level above
`win_x64/psx_xplane.xpl`, with a `win_x64/Norm.dll` override. It does not resolve
relative to X-Plane.exe. Preserve the plugin/aircraft overlay structure.

## Building the omitted NORM dependency

Use native Windows x64 MSVC and CMake with the two external archives above.
The first-party standalone recipe has no graphics, application-data or test-tool
dependency. Put its output in local build work outside maintained source:

```powershell
cmake -S scripts/norm -B <local-work>/norm -G "Visual Studio 18 2026" -A x64
cmake --build <local-work>/norm --config Release --target norm
```

The DLL/PDB are under `<local-work>/norm/norm-runtime/Release/`. Preserve upstream
NORM/Protolib licence notices when installing your separately obtained dependency.
The recipe retains the reviewed pooled-buffer, Win64 timer-key and late-object
cancellation repairs used by the coordinator/plugin. It does not accept an
arbitrary upstream replacement as evidence that those repairs are present.

Public preparation uses `scripts/prepare-release.ps1` from a qualified Release
installation. It omits NORM/fonts, saved settings, profiles, calibration,
quicksaves, customer assets and build leftovers. HWIO's required catalogue/module
XML comes from authored factory definitions, never the operator's edited copies.
The source repository remains an inventory with explicitly omitted dependencies,
not a self-contained application build distribution.

## IOS independent monitoring client

IOS consumes the external XPL Monitor static-client SDK through
`find_package(XplMonitorClient 1 CONFIG REQUIRED)` and `xpl_monitor::client`.
Select its root with `XPL_MONITOR_CLIENT_ROOT` (CMake) or
`-XplMonitorClientRoot` (IOS build/publish entry). The qualified local SDK is
`P:\development\xpl-monitor\sdk\client\1.0.0`; its headers, libraries,
compile PDBs and contract stay outside this source repository.

The selected SDK uses MSVC19.51/toolset14.51 and Windows SDK10.0.26100.0,
C++20, /MDd Debug and /MD Release. IOS and its compiled dependencies use the
matching CRT edition. Release requires the Microsoft Visual C++ x64 runtime;
internal Debug qualification uses the installed Visual Studio debug runtime.
The static client adds no NORM, XPLM, PSX or monitoring-server runtime dependency.
It reads the current user's independent collector endpoint and validates the
`xpl-watchdog.exe` peer. Host mode requires explicit endpoint selection; absent
collectors do not fall back to a coordinator-hosted monitoring service.
