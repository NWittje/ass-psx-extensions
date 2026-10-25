#pragma once
#include <psx/hwio/thrust_control.hpp>
#include <Windows.h>
namespace hwio {
struct ThrustFrame {std::uint64_t revision{};std::array<psx::hwio::thrust_control::Lever,4> levers{};std::uint32_t released_groups{};};
ThrustFrame thrust_frame(std::array<psx::hwio::thrust_control::Lever,4>,const std::array<psx::hwio::thrust_control::Lever,4>&,std::uint64_t revision,std::uint32_t released);
// Acquisition-worker state only. No pipe lifetime or PSX epoch owns a hold.
class ThrustControl {
    using Packet=psx::hwio::thrust_control::Packet;
    using Process=psx::hwio::thrust_control::Process;
    using Snapshot=psx::hwio::thrust_control::Snapshot;
    struct Request {psx::hwio::thrust_control::Operation operation{};std::uint64_t request_id{},session{},revision{},token{};std::array<double,4> percent{};
        Request()=default;explicit Request(const Packet& p):operation(p.operation),request_id(p.request_id),session(p.session),revision(p.revision),token(p.token),percent(p.percent){};};
    struct Receipt {Request request;Process peer;psx::hwio::thrust_control::Result result{};std::uint64_t token{},operation{};std::array<char,128> message{};};
    Snapshot status_;std::array<double,4> requested_{};
    std::array<Receipt,16> receipts_{};unsigned receipt_cursor_{};
    struct Client {Process peer{};std::uint64_t highwater{};};std::array<Client,16> clients_{};
    Process owner_{};HANDLE process_{};std::uint64_t next_token_{},claimed_revision_{};
    bool releasing_{},interrupted_{};std::array<char,128> cause_{};
    std::uint32_t claimed_groups_{},released_groups_{};std::uint64_t observations_{},release_after_{};
    void relinquish();void begin_release();void flags();
public:
    explicit ThrustControl(std::uint64_t session);~ThrustControl();
    ThrustControl(const ThrustControl&)=delete;ThrustControl& operator=(const ThrustControl&)=delete;
    void observe(const ThrustFrame&,std::int64_t qpc,std::uint64_t frequency);
    Packet dispatch(const Packet&,Process);
    void shutdown(){if(owns()){begin_release();flags();}}
    Snapshot snapshot(Process peer={})const noexcept {auto result=status_;if(owns() && peer!=owner_)result.flags&=~psx::hwio::thrust_control::can_set;return result;}
    bool owns()const noexcept{return status_.token!=0;}
    bool drives()const noexcept{return owns() && !releasing_ && !interrupted_;}
    std::uint64_t token()const noexcept{return status_.token;}
    std::uint64_t operation()const noexcept{return status_.operation_id;}
    std::array<double,4> targets()const noexcept;
};
}
