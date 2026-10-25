#pragma once
#include <psx/terrain/protocol.hpp>

namespace psx::xp {
struct TerrainInput {
    terrain::Query query{};
    terrain::Control control{};
    std::uint64_t connection{},revision{},lease_generation{};
    std::int64_t lease_expires_qpc{};
    bool active{},have_query{};
    bool capture_active{};
    std::uint64_t capture_generation{};
};
struct TerrainResult {
    terrain::Surface surface{};
    terrain::Reason reason{terrain::Reason::unsupported};
    std::uint64_t connection{},revision{},host_generation{},lease_generation{};
    std::uint32_t attempts{};
    double height_residual_m{},horizontal_residual_m{};
    std::array<terrain::ProbeDetail,5> details{};
};
struct TerrainCaptureResult {
    terrain::ProbeDetail probe{};
    terrain::RenderDetail render{};
    std::uint64_t connection{},lease_generation{},host_generation{};
    bool have_probe{},have_render{};
};
struct TerrainHost {
    std::uint64_t aircraft_generation{},scenery_generation{},local_frame_generation{},generation{};
    bool enabled{},loading{};
};
}
