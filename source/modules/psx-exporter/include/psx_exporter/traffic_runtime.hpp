#pragma once
#include <psx/traffic_source/protocol.hpp>
#include <string>
namespace psx_exporter {
void start_traffic(const psx::traffic_source::Hello&,const std::wstring&) noexcept;
// Worker-only fixed handoff; native publication stays unchanged.
bool own_traffic_reader() noexcept;
bool take_traffic(psx::traffic::Snapshot&) noexcept;
void publish_traffic_diagnostic(const psx::traffic::Snapshot&) noexcept;
std::uint32_t traffic_capabilities() noexcept;
psx::traffic_source::Reason traffic_reason() noexcept;
}
