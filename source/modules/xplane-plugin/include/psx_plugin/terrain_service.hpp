#pragma once
#include "terrain_types.hpp"
#include <psx/diagnostics/journal.hpp>
#include <memory>
#include <string>

namespace psx::xp {
struct TerrainOptions {std::wstring endpoint;diagnostics::DiagnosticsHub* diagnostics{};}; // Private local-pipe fixture override; default SID server.
struct TerrainServiceStatus {
    bool running{},connected{},subscribed{},active{};
    terrain::Reason reason{terrain::Reason::unsupported};
    std::uint32_t error{};
    std::uint64_t connection{},queries{},coalesced{},rejected{},surfaces{},controls{};
    std::uint64_t query_ticks_max{},source_age_ticks_max{};
};
std::wstring terrain_endpoint();
class TerrainService final {
public:
    TerrainService();~TerrainService();
    TerrainService(const TerrainService&)=delete;TerrainService& operator=(const TerrainService&)=delete;
    bool start(const TerrainOptions&,motion::InstanceId provider);
    void stop() noexcept;
    const TerrainInput& input() noexcept; // SDK owner, fixed handoff only.
    void publish(const TerrainResult&,const TerrainHost&) noexcept;
    void publish_capture(const TerrainCaptureResult&) noexcept;
    void invalidate(const TerrainHost&) noexcept; // Immediately revoke old queued replies; no waits/I/O.
    bool poll(TerrainServiceStatus&) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;TerrainInput empty_{};
};
}
