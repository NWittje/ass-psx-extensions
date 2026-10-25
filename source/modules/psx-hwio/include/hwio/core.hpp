#pragma once
#include <psx/hwio/contract.hpp>
#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <vector>
#include <string>
#include <cstddef>
#include <deque>
#include <set>

namespace hwio {
using namespace psx::hwio;
constexpr std::size_t max_usb_devices = 32, max_ethernet_devices = 32;
constexpr std::size_t max_usb_controls=512;
constexpr std::size_t first_fds_device=max_usb_devices+max_ethernet_devices, max_fds_devices=32;
constexpr std::size_t first_phidget_device=first_fds_device+max_fds_devices;
constexpr std::size_t first_generic_phidget_device=first_phidget_device+1,max_phidget_devices=32;
constexpr std::size_t first_cpflight_device=first_generic_phidget_device+max_phidget_devices;
constexpr std::size_t max_devices = first_cpflight_device+3, max_controls = 4096, max_bindings = 512;
bool same(ControlKey a, ControlKey b) noexcept;
std::string record_name(RecordKey key);
bool parse_key(std::string_view text, RecordKey &key) noexcept;
bool integer(std::string_view text, std::int32_t &value) noexcept;
bool descriptor_valid(const TargetDescriptor &d, std::string &error);
// Qh418 is a transient held-switch word, omitted from PSX startup/bang.
bool aileron_trim_command(const TargetDescriptor &) noexcept;
bool overlaps(const TargetDescriptor &a, const TargetDescriptor &b) noexcept;
const std::vector<TargetDescriptor> &catalogue();
std::optional<std::int32_t> extract(const TargetDescriptor &d, std::string_view payload);
std::optional<std::string> compose(const TargetDescriptor &d, std::string_view payload, std::int32_t value);
bool calibration_valid(const AxisCalibration &c) noexcept;
double normalize(double value, const AxisCalibration &c) noexcept;
double physical_fraction(double value, const AxisCalibration &c) noexcept;
double control_response(double value, double gain) noexcept;
class Filter {
    std::array<double, 3> history_{};
    std::size_t count_{}, pos_{};
    double target_{}, value_{}, last_{}, median_raw_{};
    bool initialized_{};

  public:
    void reset() noexcept {
        *this = Filter{};
    }
    double sample(double raw, const AxisCalibration &c, double ms) noexcept;
    double settle(const AxisCalibration &c, double ms) noexcept;
    double median_raw() const noexcept {
        return median_raw_;
    }
};
struct Source {
    ControlKey key{};
    std::string profile;
};
struct Binding {
    std::uint64_t id{};
    std::string name;
    bool enabled{true};
    std::vector<Source> sources;
    MappingMode mode{MappingMode::axis};
    AxisCalibration calibration{};
    TargetDescriptor target{};
    ThrottlePolicy throttle{ThrottlePolicy::physical_authority};
    ThrottleLayout throttle_layout{ThrottleLayout::combined};
    Source reverse_source{};
    AxisCalibration reverse_calibration{};
    AxisCalibration inactive_calibration{};
    Source inactive_source{};
    std::map<int, int> selector;
    std::vector<double> thresholds;
    double hysteresis{};
    int step{1};
    double control_stiffness{}, response_gain{1}; // Gain is compiled on configuration, never per sample.
};
struct Profile {
    std::string id, name;
    DeviceIdentity identity;
    bool explicit_path{};
    std::vector<Binding> bindings;
};
bool validate(const Profile &p, std::string &error);
std::set<std::pair<std::string, std::uint64_t>> ownership_conflicts(const std::vector<Profile> &profiles);
int identity_score(const DeviceIdentity &expected, const DeviceIdentity &actual) noexcept;
// -1 no match, -2 ambiguous. Callers also enforce unique assignment across devices.
int match_identity(const DeviceIdentity &expected, const std::vector<DeviceIdentity> &devices) noexcept;
int match_profile(const Profile &profile, const std::vector<DeviceIdentity> &devices) noexcept;
std::string profile_encode(const Profile &p);
bool profile_decode(std::string_view text, Profile &p, std::string &error);
bool save_profile(const std::wstring &path, const Profile &p, std::string &error);
bool load_profile(const std::wstring &path, Profile &p, std::string &error, bool *recovered = nullptr);
std::wstring profile_directory();
std::string utf8(std::wstring_view text);
std::wstring wide(std::string_view text);

#pragma warning(push)
#pragma warning(disable : 4324) // Intentional separation of producer/consumer cache lines.
template <class T, std::size_t N> class Spsc {
    std::array<T, N> items_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    std::size_t staged_{};
    bool batching_{},failed_{}; // Producer-owned; publication remains one release store.
    alignas(64) std::atomic<std::size_t> read_{0};

  public:
    bool push(const T &item) noexcept {
        const auto w = batching_?staged_:write_.load(std::memory_order_relaxed);
        const auto n = (w + 1) % N;
        if (n == read_.load(std::memory_order_acquire)) {
            if(batching_)failed_=true;
            return false;
        }
        items_[w] = item;
        if(batching_)staged_=n;else write_.store(n, std::memory_order_release);
        return true;
    }
    void begin_batch() noexcept {staged_=write_.load(std::memory_order_relaxed);batching_=true;failed_=false;}
    bool commit_batch() noexcept {batching_=false;if(failed_)return false;write_.store(staged_,std::memory_order_release);return true;}
    bool pop(T &item) noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        item = items_[r];
        read_.store((r + 1) % N, std::memory_order_release);
        return true;
    }
    std::size_t size() const noexcept {
        return (write_.load(std::memory_order_acquire) + N - read_.load(std::memory_order_acquire)) % N;
    }
};
#pragma warning(pop)
struct Sample {
    std::uint16_t device{}, control{};
    std::uint64_t generation{}, sequence{};
    std::int64_t qpc{};
    std::int32_t value{};
    bool baseline{}, relative{}, report_end{}, invalid{};
};
struct SourceValue {
    int value{};
    bool available{};
    std::int64_t qpc{};
    std::uint64_t sequence{};
};
struct MappingState {
    Filter filter;
    Filter reverse_filter;
    bool baseline{}, available{}, pending{}, held{}, dirty{true};
    int previous{}, desired{}, sent{}, relative_pending{}, selector_position{-1};
    double normalized{}, reverse_normalized{}, physical_forward{}, physical_reverse{}, last_event_ms{-1e9};
    std::int64_t forward_filter_qpc{}, reverse_filter_qpc{};
    std::uint64_t forward_sequence{}, reverse_sequence{};
    int forward_raw{}, reverse_raw{};
    DeliveryState delivery{DeliveryState::unavailable};
    std::int64_t input_qpc{}, mapped_qpc{}, sent_qpc{};
    std::string reason;
};
struct Mapped {
    bool externally_calculated{};
    bool reverse_unassigned{}; // Managed forward-only split throttle; distinct from a lost assigned source.
    Binding binding;
    std::string profile;
    std::uint64_t local_id{};
    std::uint16_t device{};
    std::uint64_t generation{};
    std::vector<std::uint16_t> controls;
    std::vector<std::uint16_t> source_devices;
    std::vector<std::uint64_t> source_generations;
    MappingState state;
};
void settle_axis(Mapped &mapping, double ms);
struct Record {
    std::string observed, effective, committed;
    std::int64_t qpc{};
    std::uint64_t sequence{};
    std::string transmitted;
    std::int64_t transmitted_qpc{};
};
struct Command {
    std::uint64_t binding{}, epoch{};
    std::int64_t input_qpc{}, mapped_qpc{};
    std::string line;
    bool event{};
    std::uint8_t follower_mask{};
    int value{};
    std::uint64_t cancellation_generation{};
    unsigned minimum_interval_ms{}; // Per-record wire pacing; never throttles unrelated axis records.
};
class Mapper {
    struct Decision {
        std::uint64_t binding{};
        int value{};
        std::int64_t input_qpc{};
    };
    std::deque<Decision> decisions_;
    std::map<std::string, double> last_record_event_;
    void decision(Mapped &m, int value);

  public:
    std::vector<Mapped> mappings;
    std::map<std::string, Record> records;
    std::set<std::string> watched_records; // Additional typed button/configuration records.
    std::uint64_t epoch{1};
    bool ready{};
    AtAuthority at{AtAuthority::unavailable};
    // Preserve a received native inactive edge when several PSX records are
    // drained before the motor worker runs. Disconnects are not inactive edges.
    std::uint64_t at_inactive_sequence{};
    bool overflow{};
    std::uint32_t failed_axes{}; // Ephemeral final-output overlay; physical calculation continues.
    int runaway_trim{};
    std::uint32_t trim_owned_mask{};
    bool trim_fo_owned{};
    unsigned motor_throttle_suppression{};
    bool motor_speedbrake_suppression{};
    int output_value(const TargetDescriptor&,int) const noexcept;
    void constrain_record(RecordKey,std::string&) const;
    void boundary(std::uint64_t new_epoch, bool same_connection_load = false);
    void receive(std::string_view line, std::int64_t qpc);
    void input(Mapped &m, const std::vector<SourceValue> &values, double ms, bool edge, bool baseline = false,
               bool relative = false);
    void tick(double ms);
    std::optional<Command> next(double ms, std::int64_t qpc);
    void sent(std::uint64_t id, std::int64_t qpc, std::optional<int> value = {});
    void sent_record(std::string_view line, std::int64_t qpc);
    void lost(std::uint16_t device);
};
struct Indicator {
    bool available{}, contact{}, failed{}, powered{};
    double supply{};
    bool lit{};
};
Indicator fuel_indicator(const Mapper &mapper, unsigned engine);
} // namespace hwio
