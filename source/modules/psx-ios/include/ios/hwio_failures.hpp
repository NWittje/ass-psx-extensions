#pragma once
#include "core.hpp"
#include <psx/hwio/failures.hpp>
#include <psx/instructor/instructor_metadata.hpp>
#include <psx/non_normal/pipe.hpp>
namespace ios {
// The Main worker owns this bounded, nonblocking local client.
class HwioFailures {
  using Packet=psx::hwio::failures::Packet;
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  OVERLAPPED read_{},write_{};
  bool reading_{},writing_{},awaiting_{};
  Packet incoming_{},outgoing_{};
  std::deque<Packet> pending_,replies_;
  std::optional<Packet> status_;
  std::wstring endpoint_;
  psx::non_normal::ProcessIdentity explicit_peer_{};
  Tick connect_at_{},status_at_{},reply_deadline_{};
  std::uint64_t next_{1},source_sequence_{},source_generation_{};
  bool source_valid_{};
  std::string host_,reason_{"HWIO unavailable"};
  std::uint16_t port_{};
  void accept(const Packet &);
  void io(Tick);
  bool connect();
public:
  explicit HwioFailures(std::wstring endpoint={},psx::non_normal::ProcessIdentity expected={});
  ~HwioFailures();
  void disconnected();
  void poll(Tick,bool ready,std::string host,std::uint16_t,const std::optional<psx::instructor::InstructorSample>&);
  std::uint64_t send(Packet); // Enqueue once; zero is unavailable. No automatic replay.
  bool receive(std::uint64_t,Packet &);
  bool available() const;
  const auto &status() const {return status_;}
  std::string json() const;
};
}
