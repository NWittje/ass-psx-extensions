#pragma once
#include "core.hpp"
#include "acquisition.hpp"
#include <Windows.h>
#include <hidsdi.h>
#include <mutex>
#include <thread>
#include <memory>

namespace hwio {
struct ControlInfo {
    ControlKey key{};
    int minimum{}, maximum{};
    unsigned bits{};
    bool relative{};
    std::string name;
};
struct DeviceInfo {
    DeviceIdentity identity;
    std::vector<ControlInfo> controls;
    std::uint16_t slot{};
    std::uint64_t generation{};
    bool connected{};
    bool coherent_reports{true}, complete_module{};
    bool direct{};
    unsigned report_bytes{}, input_buffers{};
    UsbInputInfo usb;
    std::string status;
};
struct AtomicSample {
    std::atomic<std::uint64_t> serial{0}, generation{0}, sequence{0};
    std::atomic<std::int64_t> qpc{0};
    std::atomic<std::int32_t> value{0};
    std::atomic<bool> invalid{false};
    void store(const Sample &s) noexcept;
    bool read(Sample &s) const noexcept;
};
// Pure report-data seam also used by descriptor/report-ID fixtures.
struct ReportValue {
    std::uint16_t index{};
    std::uint32_t value{};
};
void decode_report(const std::vector<ControlInfo> &controls, std::uint8_t report_id, const ReportValue *values,
                   std::size_t count, std::array<std::int32_t, max_controls> &out,
                   std::array<bool, max_controls> &present) noexcept;
bool report_batch(std::size_t available, std::uint32_t report_size, std::uint32_t report_count) noexcept;
bool raw_block_size(std::size_t remaining, std::uint32_t block_size) noexcept;
struct InputActivity {
    std::uint64_t reports{}, packets{};
    std::int64_t last_qpc{};
};
class HidInput {
    friend struct RuntimeProbe;
    friend struct HidInputProbe;
    struct Cached {
        HANDLE handle{};
        DeviceInfo info;
        std::vector<std::uint8_t> preparsed;
        std::array<bool, max_usb_controls> known{};
        std::array<int, max_usb_controls> values{};
        std::unique_ptr<AsyncInput> stream;
        bool raw_ready{};
    };
    std::array<Cached, max_usb_devices> devices_;
    std::array<std::array<AtomicSample, max_usb_controls>, max_usb_devices> latest_;
    std::array<std::atomic<std::uint64_t>,max_usb_devices> report_revision_{};
    struct AtomicActivity {
        std::atomic<std::uint64_t> reports{0}, packets{0};
        std::atomic<std::int64_t> last_qpc{0};
    };
    std::array<AtomicActivity, max_usb_devices> activity_;
    std::thread thread_;
    std::thread discovery_thread_;
    HANDLE stop_event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE discover_event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    HANDLE discovered_event_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex discovery_mutex_;
    std::vector<Cached> discovered_;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stop_{false};
    std::mutex metadata_mutex_;
    std::vector<DeviceInfo> metadata_;
    alignas(RAWINPUT) std::array<std::uint8_t, 65536> raw_{};
    std::array<HIDP_DATA, 4096> parsed_{};
    std::array<ReportValue, 4096> values_{};
    std::uint64_t generation_{}, sequence_{};
    HWND window_{};
    void run();
    void discover();
    void discovery_run();
    void adopt_discovered();
    void open_direct(Cached &device);
    void complete_direct(Cached &device);
    void fallback(Cached &device, DWORD error);
    bool report(Cached &device, const std::uint8_t *data, DWORD bytes, std::int64_t stamp);
    void decoded(Cached &device, const ReportValue *values, std::size_t count, std::uint8_t id, std::int64_t stamp);
    void publish_metadata();
    void raw(HRAWINPUT handle);
    void consume(const RAWINPUT *input, std::size_t bytes, std::int64_t stamp);
    void drain();
    void remove(HANDLE handle);
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);

  public:
    HANDLE available_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Spsc<Sample, 4096> edges;
    std::atomic<std::uint64_t> overflow{0}, reports{0}, rejected{0}, metadata_revision{0};
    std::atomic<std::uint64_t> buffered_packets{0}, buffer_reads{0};
    HidInput() = default;
    std::uint64_t report_revision(std::size_t slot) const noexcept {return slot<max_usb_devices?report_revision_[slot].load(std::memory_order_acquire):1;}
    ~HidInput();
    void start();
    void stop();
    std::vector<DeviceInfo> devices();
    bool latest(std::size_t device, std::size_t control, Sample &value) const noexcept;
    InputActivity activity(std::size_t device) const noexcept;
};
} // namespace hwio
