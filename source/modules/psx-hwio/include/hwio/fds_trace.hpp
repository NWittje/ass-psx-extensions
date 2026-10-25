#pragma once
#include <Windows.h>
#include <array>
#include <vector>
#include <mutex>
#include <filesystem>
#include <string_view>
#include <span>
#include <deque>
#include <string>
#include <thread>
#include <atomic>
namespace hwio::fds {
enum class TraceKind:std::uint16_t {usb_input,api_result,tcp_rx,tcp_tx,metadata,psx_record,routing,output,fault,udp_rx,udp_tx,context,input,session,usb_bitmap};
struct TraceEntry {std::uint64_t stamp{},sequence{},generation{};std::uint32_t stream{},event{};TraceKind kind{};std::uint16_t size{};std::array<char,220> bytes{};};
static_assert(sizeof(TraceEntry)==256);
class Trace {
    mutable std::mutex mutex_;std::vector<TraceEntry> ring_{65536};std::uint64_t written_{};std::uint32_t event_{};std::filesystem::path fault_directory_;unsigned fault_index_{};
    std::deque<std::pair<std::string,std::vector<TraceEntry>>> faults_;
    std::atomic<bool> live_stop_{true};std::thread live_worker_;
public:
    ~Trace();void live(bool);
    std::vector<TraceEntry> since(std::uint64_t&,std::uint64_t&,unsigned limit=32)const;
    void record(TraceKind,std::uint32_t,std::uint64_t,std::string_view,std::uint64_t stamp=0);
    std::vector<TraceEntry> snapshot() const;
    std::uint64_t written() const;static void write(const std::filesystem::path&,std::span<const TraceEntry>);
    void fault(std::string);std::vector<std::pair<std::string,std::vector<TraceEntry>>> faults() const;
    static std::vector<TraceEntry> read(const std::filesystem::path&);
    void fault_directory(std::filesystem::path);
    static std::uint64_t frequency(const std::filesystem::path&);
};
}
