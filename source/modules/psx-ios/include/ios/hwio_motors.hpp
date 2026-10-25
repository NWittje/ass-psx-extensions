#pragma once
#include "core.hpp"
#include <psx/hwio/motors.hpp>
#include <psx/hwio/actuators.hpp>
#include <psx/hwio/situation_controls.hpp>
#include <psx/instructor/instructor_metadata.hpp>
#include <psx/non_normal/pipe.hpp>
namespace ios {
// The Main worker owns this bounded, nonblocking local client.
class HwioMotors {
  using Packet=psx::hwio::actuators::Packet;
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  OVERLAPPED read_{},write_{};
  bool reading_{},writing_{},awaiting_{};
  struct Buffers {
    Packet incoming{},outgoing{};
    psx::hwio::motors::v2::Packet v2_incoming{},v2_outgoing{};
    psx::hwio::motors::Packet legacy_incoming{},legacy_outgoing{};
    std::optional<Packet> status;
  };
  std::unique_ptr<Buffers> buffers_;
  Packet &incoming_, &outgoing_;
  psx::hwio::motors::v2::Packet &v2_incoming_, &v2_outgoing_;
  psx::hwio::motors::Packet &legacy_incoming_, &legacy_outgoing_;
  std::deque<Packet> pending_,replies_;
  std::optional<Packet>& status_;
  std::wstring endpoint_,v2_endpoint_,v3_endpoint_;
  psx::non_normal::ProcessIdentity explicit_peer_{};
  Tick connect_at_{},status_at_{},reply_deadline_{};
  std::uint64_t next_{1},connection_{};
  std::uint16_t wire_version_{};
  struct Action {std::uint64_t request{},operation{},session{},revision{},generation{},connection{};Tick since{};};
  std::array<Action,60> actions_;
  std::string host_,reason_{"HWIO unavailable"};
  std::uint16_t port_{};
  void accept(const Packet &);
  void accept_read();
  void io(Tick);
  bool connect();
  unsigned wire_slots() const;
  DWORD wire_bytes() const;
public:
  explicit HwioMotors(std::wstring endpoint={},psx::non_normal::ProcessIdentity expected={},std::wstring v2_endpoint={},std::wstring v3_endpoint={});
  ~HwioMotors();
  void disconnected();
  void poll(Tick,std::string host,std::uint16_t);
  void command(Operation&,Tick,bool first);
  std::uint64_t send(Packet); // Enqueue once; zero is unavailable. No automatic replay.
  bool receive(std::uint64_t,Packet &);
  std::uint64_t send(psx::hwio::motors::v2::Packet);
  bool receive(std::uint64_t,psx::hwio::motors::v2::Packet&);
  bool available() const;
  std::optional<psx::hwio::situation_controls::Snapshot> situation_controls() const;
  const auto &status() const {return status_;}
  std::string json() const;
  std::string actuator_json()const;
};
}
