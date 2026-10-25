#pragma once
#include "hid.hpp"
#include "ethernet.hpp"
#include <filesystem>
#include "fds_trace.hpp"
#include "fds_cdu.hpp"

namespace hwio::fds {
constexpr std::size_t controls_bound=512, roots_bound=16;
// Exact-build SDK bank acquisition, performed by the output worker before a
// first dataline write. Constructor seam permits authored transport fixtures.
using DatalineBaseline=int(*)(HMODULE,DWORD);
int verified_dataline_baseline(HMODULE,DWORD);
// Reserved Boolean backlight endpoint; 331A LCD power uses its advertised
// native dataline6, sharing bank0 with the annunciators.
constexpr unsigned cdu_backlight_channel=253;
constexpr unsigned cdu_lcd_power_channel=6;
// Original ABI declaration, independently checked against the installed x64
// export callers. Sessions are DWORDs even in a 64-bit process.
struct BoardCaps {
    std::array<int,21> resources{};
    char model[5]{}, manufacture_date[9]{};
    DWORD features{};
    int update{};
};
static_assert(sizeof(BoardCaps)==108 && offsetof(BoardCaps,features)==100 && offsetof(BoardCaps,update)==104);
bool valid_caps(const BoardCaps&) noexcept;
bool analogue_bank(const BoardCaps&) noexcept;
bool generic_model(std::string_view) noexcept;
std::string_view model_name(std::string_view) noexcept;
std::string usb_key(std::string_view serial);
// Only the fingerprint-qualified API uses padded 13-byte identity slots.
// Omit incomplete physical identities; never manufacture a serial suffix.
bool usb_serial_list(std::string_view bytes,bool fixed_slots,std::vector<std::string>& serials,unsigned& omitted,std::string& error);
struct Device {
    DeviceInfo input;
    BoardCaps caps;
    std::string key, root, model, status;
    std::vector<HardwareResource> resources;
    bool baseline{}, generic{}, readouts_known{};
    bool outputs_pending{};
    unsigned child{};
};
struct Output {std::uint16_t slot{};HardwareOutputIntent intent;double failure{};bool release{};};
bool valid_usb_output(const Device&,const Output&) noexcept;
struct Health {std::string usb, ethernet;std::uint64_t revision{};};
// UDP/TCP parsing is independent of sockets and tested with fragmented input.
struct Discovery {std::string name, model;};
bool parse_discovery(std::string_view,Discovery&,std::string&);
struct Message {
    enum class Kind {other, identity, board, switch_value, rotary, error, snapshot, state, usage, configuration, firmware, prompt, analogue_raw} kind{};
    // analogue_raw preserves the wire integer for diagnostics only. It has no
    // qualified ADC range, calibration, baseline or routing authority.
    unsigned board{}, index{};
    int value{};
    std::string text, model, serial;
    struct Range {ResourceKind kind{};unsigned count{},first{},last{};} range;
    std::string firmware,revision;
    std::array<std::string,2> descriptor;
};
bool parse_line(std::string_view,Message&,std::string&);
// Bounded session metadata. Discovery names and BOARD's opaque strings are
// never substituted for SERIAL. This inventory does not establish baselines.
struct NetworkBoard {
    bool described{}, firmware_seen{};
    std::string model,firmware,revision;
    std::array<std::string,2> descriptor;
    std::array<Message::Range,6> ranges{};
    std::array<bool,6> declared{};
};
struct NetworkInventory {
    std::string serial,prompt,problem;
    std::array<NetworkBoard,32> boards{};
    int state{-1},usage{-1};
    std::uint64_t revision{};
    bool accept(const Message&);
    void reset();
    bool invalidate(std::string);
    unsigned board_count() const noexcept;
    unsigned resource_count() const noexcept;
    bool output_ready(unsigned board) const noexcept;
    std::string status() const;
};
bool network_output_command(const Device&,const Output&,std::string&);
bool invalidate_duplicate_roots(std::span<NetworkInventory* const>);
class Framer {
    std::array<char,2048> line_{};
    std::size_t size_{};
    bool discard_{};
public:
    template<class F> void feed(std::span<const std::uint8_t> bytes,F&& accept) {
        for(const auto byte:bytes) {
            if(byte=='\r' || byte=='\n') {
                if(size_ && !discard_)accept(std::string_view(line_.data(),size_));
                size_=0;discard_=false;
            } else if(byte<32 || byte>126 || size_==line_.size()) {discard_=true;size_=0;}
            else if(!discard_)line_[size_++]=static_cast<char>(byte);
        }
    }
    void reset() noexcept {size_=0;discard_=false;}
};
class Provider {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Provider(Cdu3302Factory=native_cdu3302_port,DatalineBaseline=verified_dataline_baseline);~Provider();
    Provider(const Provider&)=delete;
    HANDLE available_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    Spsc<Sample,8192> edges;
    std::atomic<std::uint64_t> overflow{},reports{},rejected{},metadata_revision{};
    void start(std::wstring dll,std::wstring adapter,std::vector<std::string> disabled={});
    void configure(std::wstring dll,std::wstring adapter,std::vector<std::string> disabled={});
    void stop();
    void discover();
    std::vector<Device> inventory();
    std::vector<NetworkInventory> network_descriptors();
    std::vector<DeviceInfo> devices();
    std::vector<EthernetHealth> generic_health();
    Health health();
    bool latest(std::size_t,std::size_t,Sample&) const noexcept;
    InputActivity activity(std::size_t) const noexcept;
    bool submit(Output) noexcept;
    // Offline replay consumes the same output queue without loading an SDK or opening sockets.
    std::vector<Output> offline_outputs();
    Trace trace;
    struct Identification {std::string device;std::vector<unsigned> lamps;bool brightness{},on{},active{};std::uint64_t generation{};bool allow_readout_blanking{};};
    void identify(Identification);
    bool identify_outputs(IdentificationRequest,const IdentificationCard&);
    std::optional<IdentificationCompletion> identification_completion(std::uint64_t session);
    void enabled(bool);
    // Release precisely these resources; never a controller-wide CLEAR/test.
    void release(std::span<const Output>);
};
} // namespace hwio::fds
