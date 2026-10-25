#pragma once
#include <psx/bridge/protocol.hpp>
#include <array>
namespace coordinator {
struct MotionSnapshot {
    bool connected{},identity_available{},status_available{},last_ack_local{},transport_fault{};
    std::uint64_t connection_id{},queued{},outstanding{},cancelled{},late_acks{},control_overflows{};
    std::int64_t status_received_qpc{};
    psx::bridge::MotionRequest last_request;
    psx::bridge::MotionAck last_ack;
    psx::bridge::MotionStatus status;
};
enum class MotionRecordKind:std::uint8_t {request=10,ack=11,status=12};
struct MotionRecord {
    MotionRecordKind kind{MotionRecordKind::request};bool local{};std::int64_t received_qpc{};psx::bridge::Frame frame;
};
// Pure bounded state machine. Facade/bridge/acquisition serialize calls with
// their existing short mutex. Nothing here performs I/O or replays a command.
class MotionControl {
public:
    static constexpr std::size_t operation_capacity=16,record_capacity=64;
    void attach(std::uint64_t connection,std::int64_t now);
    void detach(std::int64_t now,psx::motion::Reason=psx::motion::Reason::disconnected);
    void plugin(const psx::bridge::PluginStatus&,std::int64_t now);
    std::uint64_t request(psx::motion::Action,const psx::bridge::LatestState&,std::int64_t now,psx::motion::ReconstructionSettings settings={},
        psx::motion::SourcePreference preference=psx::motion::SourcePreference::automatic,const psx::bridge::SourceSelection* selection=nullptr,bool exporter_ready=false);
    bool next_request(psx::bridge::MotionRequest&);
    bool acknowledge(const psx::bridge::MotionAck&,std::int64_t received);
    bool status(const psx::bridge::MotionStatus&,std::int64_t received);
    bool next_record(MotionRecord&);
    const MotionSnapshot& snapshot() const noexcept{return view_;}
private:
    struct Pending {bool active{},sent{},accepted{};psx::bridge::MotionRequest request;};
    bool record(MotionRecordKind,const psx::bridge::Frame&,std::int64_t,bool local=false,bool reserved=false);
    void fault(std::int64_t now);
    bool identities(std::uint64_t load,std::uint64_t enable,std::uint64_t aircraft,std::int64_t now);
    void cancel(std::int64_t now,psx::motion::Reason);
    void remember(const psx::bridge::MotionRequest&);
    std::array<Pending,operation_capacity> pending_{};
    std::array<psx::bridge::MotionRequest,record_capacity> retired_{};
    std::size_t retired_next_{};
    std::array<std::size_t,operation_capacity> queue_{};std::size_t queue_head_{},queue_size_{};
    std::array<MotionRecord,record_capacity> records_{};std::size_t record_head_{},record_size_{};
    std::uint64_t operation_{},load_{},enable_{},aircraft_{};
    MotionSnapshot view_;
};
const char* motion_phase_name(psx::motion::Phase) noexcept;
const char* motion_reason_name(psx::motion::Reason) noexcept;
}
