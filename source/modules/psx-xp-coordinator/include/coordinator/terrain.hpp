#pragma once
#include "coordinator/exporter.hpp"
#include <psx/terrain/protocol.hpp>
#include <array>
#include <memory>
#include <string>

namespace coordinator {
enum class TerrainEventKind : std::uint8_t {frame, lost};
struct TerrainEvent {
    TerrainEventKind kind{TerrainEventKind::frame};
    psx::terrain::Role role{psx::terrain::Role::plugin};
    ProcessIdentity process{};
    std::int64_t received_qpc{};
    std::uint32_t error{};
    std::uint64_t connection_generation{};
    psx::terrain::Frame frame{};
};
struct TerrainPeerView {
    bool connected{},owned{},pending{};
    psx::terrain::Hello hello{};
    std::int64_t expires_qpc{},ack_qpc{};
    std::uint32_t error{};
    std::uint64_t connection_generation{};
};
struct TerrainView {
    bool following{},physical_available{},physical_fresh{},surface_available{},consumed_available{},diagnostics_available{};
    std::uint64_t owner{},terrain_epoch{1},physical_revision{},queries{},surfaces{},rejected{},controls{},acks{},overflows{},boundaries{};
    TerrainPeerView plugin{},exporter{};
    psx::terrain::Reference reference{};
    psx::terrain::Surface surface{};
    psx::terrain::Consumed consumed{};
    psx::terrain::Diagnostics diagnostics{};
    std::int64_t surface_received_qpc{},consumed_received_qpc{};
    psx::terrain::Phase phase{psx::terrain::Phase::unavailable};
    psx::terrain::Reason reason{psx::terrain::Reason::unsupported};
};
// Pure, bounded acquisition-owner reducer. No SDK, transport, file or heap work.
class TerrainModel {
public:
    void configure(std::uint64_t frequency,std::uint64_t owner) noexcept;
    void physical(const PhysicalPoseCache&,std::int64_t now) noexcept;
    void following(bool,std::int64_t now) noexcept;
    void capture(bool enabled,std::int64_t now) noexcept;
    std::uint64_t capture_generation() const noexcept{return capture_generation_;}
    bool capture_detail(const TerrainEvent&) const noexcept;
    void provider_boundary(std::int64_t now) noexcept{boundary(psx::terrain::Reason::scenery,now);for(auto& l:lanes_)l.blocked=false;}
    bool event(const TerrainEvent&) noexcept;
    void tick(std::int64_t now) noexcept;
    bool next(psx::terrain::Role,psx::terrain::Frame&,std::int64_t now) noexcept;
    const TerrainView& view() const noexcept{return view_;}
private:
    struct ControlEntry {psx::terrain::Control value{};std::int64_t sent_qpc{};};
    struct Lane {
        std::array<ControlEntry,16> controls{},awaiting{};
        std::size_t head{},size{},waiting{};
        std::int64_t renewed_qpc{};
        bool blocked{};
        psx::terrain::Reason blocked_reason{psx::terrain::Reason::none};
        std::int64_t blocked_at{};
    };
    struct Pending {psx::terrain::Query query{};bool active{};};
    bool fresh(std::int64_t now,std::int64_t time,std::uint32_t ms) const noexcept;
    void release(psx::terrain::Reason,std::int64_t now) noexcept;
    void boundary(psx::terrain::Reason,std::int64_t now) noexcept;
    bool control(std::size_t,psx::terrain::Action,std::int64_t now) noexcept;
    void lost(std::size_t,std::uint32_t error,std::int64_t now) noexcept;
    TerrainPeerView& peer(std::size_t i) noexcept{return i?view_.exporter:view_.plugin;}
    std::uint64_t frequency_{},operation_{},query_id_{};
    std::int64_t last_query_qpc_{};
    TerrainView view_{};
    std::array<Lane,2> lanes_{};
    std::array<Pending,16> pending_{};
    psx::terrain::Query query_{};bool query_ready_{};
    psx::terrain::Surface surface_{};bool surface_ready_{};
    bool generations_known_{};
    std::uint64_t aircraft_generation_{},scenery_generation_{},local_generation_{};
    bool capture_enabled_{};
    std::uint64_t capture_generation_{1};
    std::int64_t capture_started_qpc_{};
    std::array<std::int64_t,2> capture_peer_started_qpc_{};
    std::array<bool,2> capture_ready_{};
};

std::wstring terrain_plugin_pipe_name();
std::wstring terrain_exporter_pipe_name(ProcessIdentity);
ProcessIdentity terrain_process_identity(std::uint32_t pid) noexcept;
// Dedicated I/O owner; two independently reconnecting authenticated lanes.
class TerrainService {
public:
    TerrainService();~TerrainService();
    void start(std::uint64_t frequency,std::uint64_t owner);
    void associate(ProcessIdentity); // no reset on Main loss/display preference
    void plugin(ProcessIdentity);
    void capture(bool enabled,std::uint64_t generation,std::int64_t now) noexcept;
    void stop() noexcept;
    bool pop(TerrainEvent&);
    bool send(psx::terrain::Role,const psx::terrain::Frame&);
    void fault(psx::terrain::Role,std::uint32_t error) noexcept;
    std::uintptr_t ready_event() const noexcept;
    std::uint64_t dropped() const noexcept;
private:
    friend struct TerrainTestAccess;
    friend class ObservationController;
    void fixture_names(std::wstring plugin,std::wstring exporter);
    bool fixture_event(const TerrainEvent&);
    bool fixture_output(psx::terrain::Role,psx::terrain::Frame&);
    struct Impl;std::unique_ptr<Impl> impl_;
};
const char* terrain_phase_name(psx::terrain::Phase) noexcept;
const char* terrain_reason_name(psx::terrain::Reason) noexcept;
}
