#pragma once
#include <psx/aircraft_sync/protocol.hpp>
#include <string>
namespace psx_exporter {
void start_aircraft(const psx::aircraft_sync::Hello&,const std::wstring&) noexcept;
}
