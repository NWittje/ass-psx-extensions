#pragma once
#include <WinSock2.h>
#include <psx/display/source_counters.hpp>
#include <Windows.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
namespace psx::du::transport {
inline constexpr size_t frame_limit=8*1024*1024, envelope_limit=4096;
using Bytes=std::vector<uint8_t>;
using Owned=std::shared_ptr<const Bytes>;
uint64_t qpc(); uint64_t frequency();
struct Source { std::wstring pipe,label; uint32_t pid{}; uint64_t created{}; };
std::vector<Source> discover(unsigned unit=0);
bool identity_json(std::string_view, bool* multi=nullptr);
struct Origin { uint32_t pid{}; uint64_t created{},clock{}; std::string identity; };
struct LocalView { Origin origin; std::wstring status; uint64_t epoch{},received{},lost{},invalid{}; bool connected{}; SourceCounters counters; std::array<wchar_t,256> rejection{}; uint64_t rejected_sequence{},rejected_qpc{}; };
class Trace;
struct OwnedSink { std::function<void(Owned,const LocalView&)> frame; Trace* trace{}; unsigned producer{}; };
struct InstrumentSourceTag {};
// One pipe worker. The sink must only validate/copy into a bounded latest mailbox.
class LocalSource {
 std::thread worker_; std::atomic<bool> quit_{};
 mutable std::mutex mutex_; LocalView view_; std::wstring selected_;
 unsigned unit_; std::function<void(std::span<const uint8_t>,const LocalView&)> sink_;
 bool instrument_{};
 OwnedSink owned_sink_;
 void run(); void status(std::wstring,bool=false);
public:
 LocalSource(unsigned,std::wstring,std::function<void(std::span<const uint8_t>,const LocalView&)>);
 LocalSource(unsigned,std::wstring,OwnedSink);
 LocalSource(unsigned,std::wstring,OwnedSink,InstrumentSourceTag);
 ~LocalSource(); LocalSource(const LocalSource&)=delete;
 void request_stop() noexcept; // Signal only: joins and pipe teardown stay in stop().
 void stop(); LocalView view() const;
};
enum class State:uint32_t { frame=1, waiting=2, busy=3, disconnected=4, invalid=5, disabled=6 };
struct Envelope {
 std::array<uint8_t,16> session{}; Origin source;
 uint32_t unit{}; State state{State::frame}; uint64_t generation{},sequence{},offered{};
 std::span<const uint8_t> frame;
};
Owned encode(const Envelope&); Envelope decode(std::span<const uint8_t>);
std::string group(unsigned unit); uint16_t port(unsigned unit);
// Enforces one stream generation, strict order, and bounded retired-host identities.
class Admission {
 std::array<std::array<uint8_t,16>,32> retired_{}; size_t count_{};
 std::array<uint8_t,16> current_{}; Origin source_;
 uint64_t generation_{},sequence_{}; bool active_{};
public:
 bool accept(const Envelope&); void reset();
};
}
