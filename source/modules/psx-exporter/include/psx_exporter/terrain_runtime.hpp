#pragma once
#include <psx/terrain/protocol.hpp>
#include <string>
namespace psx_exporter {
void start_terrain(const psx::terrain::Hello&,const std::wstring&) noexcept;
}
