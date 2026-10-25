# Omitted dependencies and data locations

Paths are relative to this repository unless a build variable is named. This
initial source import contains our application/shared code, first-party build
helpers and project icons. ASS Graphics remains a separate project. Vendor code,
compiled files, fonts, datasets, catalogues, tests, standalone diagnostic tools
and engineering documentation are intentionally absent.

| Omitted input | Intended position |
|---|---|
| ASS Graphics SDK | Outside the repository, selected with `ASS_GRAPHICS_SDK_ROOT` / `AssGraphics_DIR`. Keep the matched Debug/Release `include`, `lib`, `bin`, `symbols` and required supplier notices. Current DU/CDU/coordinator consumers pin 1.4.2; HWIO selects 1.4.3 or newer compatible input. No graphics implementation or SDK is included here. |
| X-Plane SDK 4.3 | `source/external/xplane-sdk-4.3/`, including `CHeaders/XPLM` and `Libraries/Win/XPLM_64.lib`, or the explicit `XPLANE_SDK_ROOT`. |
| LZMA SDK 26.05 | `source/external/lzma-sdk-26.05/`: qualified decoder C sources/headers and the matching `SHA256SUMS` expected by the plugin build. |
| DU/CDU NORM supplier inputs | `source/shared/norm/vendor/norm/include/` for headers; `source/shared/norm/vendor/norm/runtime/` for `Norm.dll` and its required matching Microsoft runtime DLLs. Keep supplier licences separately. |
| Coordinator/plugin NORM source inputs | `source/shared/sync-network/vendor/norm-v1.5.9.tar.gz` and `protolib-702e9099.tar.gz`. The included first-party CMake recipe checks their pinned hashes and applies our reviewed maintenance. Its extracted upstream code and built DLL/PDB remain in the build directory. These inputs are separate from the DU/CDU supplier DLL. |
| CivetWeb | `source/modules/psx-ios/vendor/civetweb/include/` and `source/modules/psx-ios/vendor/civetweb/src/civetweb.c`, with the supplier's notices. |
| B612 font files | `source/modules/psx-ios/vendor/fonts/B612-Regular.ttf` and `B612-Bold.ttf`. Fonts used internally by ASS Graphics stay with that external supplier input. |
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
