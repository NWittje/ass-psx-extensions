#pragma once
#include <psx/terrain/protocol.hpp>
#include <cstdint>
#include <memory>
#include <string>
namespace coordinator {
struct TerrainEvidenceStatus {bool active{},complete{},incomplete{};std::uint64_t records{},bytes{},overflows{};std::uint32_t error{};};
// Independent opt-in sidecar; all filesystem work belongs to its file worker.
class TerrainEvidence {
public:
    TerrainEvidence();~TerrainEvidence();
    bool prepare(const std::wstring&,std::uint64_t frequency); // Accept request; deferred errors are in status().
    void begin(std::int64_t now);
    // type 1 inbound PXT, 2 outbound PXT, 5 original raw PXSE frame.
    void record(std::uint8_t type,psx::terrain::Role,const std::uint8_t*,std::size_t,std::int64_t);
    void finish(std::int64_t now) noexcept;
    void incomplete(std::uint32_t error,std::int64_t now) noexcept;
    void join();
    bool done() const noexcept;
    TerrainEvidenceStatus status() const;
private:
    friend struct EvidenceTestAccess;
    bool test_open_hook(void (*)(void*) noexcept,void*);
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
