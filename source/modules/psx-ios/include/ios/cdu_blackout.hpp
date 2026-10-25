#pragma once
#include "core.hpp"
#include <psx/cdu/blackout.hpp>
namespace ios {
struct CduBlackoutResult {
  std::uint64_t request{},revision{};
  unsigned mask{};
  std::string state,reason;
  std::string json() const;
};
// Main-worker owned. Discovery never binds or restores a previous blackout.
class CduBlackout {
  using Packet=psx::cdu::blackout::Packet;
  HANDLE pipe_{INVALID_HANDLE_VALUE},process_{};
  OVERLAPPED read_{},write_{};
  std::array<std::uint8_t,psx::cdu::blackout::pipe_bytes> incoming_{},outgoing_{};
  bool reading_{},writing_{},awaiting_{},bound_{},mutated_{};
  DWORD pid_{};
  std::uint64_t creation_{},next_wire_{1},next_action_{1},action_{};
  std::optional<Packet> status_,expected_;
  Packet sent_;
  enum class Purpose { background, discovery, bind, mutation };
  Purpose purpose_{Purpose::background};
  enum class Step { idle, discovery, bind, mutation, waiting };
  Step step_{Step::idle};
  unsigned selected_{};
  bool enabled_{},clear_all_{};
  std::optional<CduBlackoutResult> result_;
  std::wstring endpoint_;
  DWORD fixture_pid_{};
  std::string host_,reason_{"CDU host unavailable"};
  std::uint16_t port_{};
  Tick connect_at_{},status_at_{},deadline_{};
  std::size_t candidate_{};
  bool connect();
  bool identity() const;
  bool matches(const Packet &) const;
  void close();
  void lost(std::string);
  void finish(std::string,std::string);
  void transmit(Packet,Purpose,Tick);
  void accept(const Packet &);
  void io(Tick);
public:
  explicit CduBlackout(std::wstring authored_endpoint={},DWORD authored_pid=0);
  ~CduBlackout();
  CduBlackout(const CduBlackout &)=delete;
  CduBlackout &operator=(const CduBlackout &)=delete;
  void disconnected();
  void poll(Tick,bool main_ready,std::string host,std::uint16_t port);
  bool available() const;
  // A new explicit action may bind fresh state. Zero means it was not queued.
  std::uint64_t request(unsigned selected,bool enabled,bool clear_all=false);
  bool receive(std::uint64_t,CduBlackoutResult &);
  std::string json() const;
};
}
