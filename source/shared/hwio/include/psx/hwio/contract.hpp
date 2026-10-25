#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace psx::hwio {
// In-process contracts, NOT an ABI or a serialized wire representation.
inline constexpr std::uint32_t profile_revision = 1;
inline constexpr std::uint32_t semantic_revision = 1;
inline constexpr std::uint16_t default_main_port = 10747;
inline constexpr std::uint32_t delta_min_interval_ms = 50;
inline constexpr std::array<int, 8> autobrake_values{0,1,2,3,4,5,6,7};
inline constexpr std::array<std::string_view, 8> autobrake_names{"RTO","OFF","DISARM","1","2","3","4","MAX"};
inline constexpr std::array<int, 7> flap_values{0,1,2,3,4,5,6};
inline constexpr std::array<std::string_view, 7> flap_names{"UP","1","5","10","20","25","30"};

enum class RecordKind : std::uint8_t { qh, qi, qs };
enum class ValueLayout : std::uint8_t { integer, bit_field, semicolon_field, fixed_decimal };
enum class Semantics : std::uint8_t { absolute, held, pulse, relative, read_only };
enum class MappingMode : std::uint8_t { axis, momentary, maintained, toggle, selector, relative };
enum class ControlKind : std::uint8_t { axis, button, hat };
// Values 0/1 are persisted by released HID/PoKeys profiles. Never renumber.
enum class BackendKind : std::uint8_t { usb_hid=0, pokeys_ethernet=1, phidget22=2, fds_usb=3, fds_ethernet=4, cpflight_serial=5 };
enum class ResourceKind : std::uint16_t {
    // Released PoKeys resource addresses remain byte-for-byte compatible.
    digital=1, analog, encoder, counter, matrix, sensor, ppm, poil, ponet,
    pwm, led, lcd, poext, i2c, spi, onewire, can, uart, motion, encoder_count,
    boolean_input=100, boolean_output, rotary_delta, analogue_input,
    integral_display, dataline_output, servo_output
};
struct ResourceAddress {
    ResourceKind kind{};
    std::uint16_t channel{}, child{};
    friend constexpr bool operator==(ResourceAddress,ResourceAddress)=default;
};
enum class ThrottlePolicy : std::uint8_t { physical_authority, at_servo_follower };
enum class ThrottleLayout : std::uint8_t { combined, separate_forward_reverse };
enum class AtAuthority : std::uint8_t { unavailable, physical, servo };
enum class CommandCause : std::uint8_t { hardware_change, baseline, reconciliation, release };
enum class DeliveryState : std::uint8_t { unavailable, queued, sent, observed };

struct RecordKey {
    RecordKind kind{RecordKind::qh}; std::uint16_t index{};
    friend constexpr bool operator==(RecordKey a, RecordKey b) noexcept { return a.kind==b.kind && a.index==b.index; }
};
struct ControlKey {
    ControlKind kind{ControlKind::axis};
    std::uint16_t usage_page{}, usage{}, link_collection{}, data_index{};
    std::uint8_t report_id{};
    BackendKind backend{BackendKind::usb_hid};
    std::uint16_t channel_kind{}, channel{};
};
// Persist evidence, not Raw Input handles or enumeration order. Strings are
// configured/discovered off the report-processing path.
struct DeviceIdentity {
    std::uint16_t vendor{}, product{}, revision{}, usage_page{}, usage{};
    std::string serial, container_id, name, descriptor_signature, collection_identity, instance_path;
    BackendKind backend{BackendKind::usb_hid};
    std::string hardware_key; // Provider-qualified persistent identity; never an IP/slot.
};
struct AxisCalibration {
    double minimum{-32768}, center{0}, maximum{32767};
    // Deadzone widths are in RAW DEVICE COUNTS, not percentages or PSX units.
    double dead_center_low{}, dead_center_high{}, dead_minimum{}, dead_maximum{};
    bool centered{true}, inverted{false}, median_three{false};
    // 0 disables filtering. Enabled range is 1..100 milliseconds.
    double lowpass_ms{};
};
struct TargetDescriptor {
    std::string id, name, unit;
    RecordKey record{};
    ValueLayout layout{ValueLayout::integer};
    Semantics semantics{Semantics::absolute};
    std::int32_t minimum{}, maximum{}, released{}, pressed{1};
    // For a bit_field, value is shifted into owned_mask. Never copy a received
    // transient pulse into a new command. Preserve all other received bits.
    std::uint32_t owned_mask{}, transient_mask{};
    std::uint8_t shift{};
    // fixed_decimal uses a byte offset and decimal width; earlier layouts keep
    // their original field/index meanings and persisted enum values.
    std::uint16_t field{}, field_count{};
    // -1 is not a throttle. 0..3 addresses the four independent TLA fields.
    std::int8_t throttle_engine{-1};
    bool writable{true};
};
struct CommandIntent {
    std::uint64_t binding{}, session_epoch{}, device_generation{}, sequence{};
    std::int64_t input_qpc{};
    std::int32_t value{};
    CommandCause cause{CommandCause::hardware_change};
};
struct OutputObservation {
    std::uint64_t session_epoch{}, sequence{};
    std::int64_t receive_qpc{};
    double value{};
    bool available{};
};
struct HardwareOutputIntent {
    std::uint64_t session_epoch{}, device_generation{}, sequence{};
    std::int64_t source_qpc{};
    std::uint16_t resource{}, channel{};
    double value{};
    std::array<char,128> text{};
    bool available{}, stop{}, velocity{};
};
// Main-TCP command ownership is separate from physical output delivery.
// OutputObservation remains observation-only; the Ethernet provider accepts
// explicitly configured HardwareOutputIntent values with session identity.
class IntentSink {
public:
    virtual ~IntentSink() = default;
    virtual bool submit(const CommandIntent&) noexcept = 0;
};
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void observe(std::string_view target, const OutputObservation&) noexcept = 0;
};

constexpr AtAuthority at_authority(int mode) noexcept {
    // Installed Qs434 field 0 / AfdsHub.readGlob; PFD labels HOLD as 21.
    return mode==0 || mode==21 ? AtAuthority::physical :
           (mode>=18 && mode<=22 ? AtAuthority::servo : AtAuthority::unavailable);
}
constexpr std::uint32_t merge_owned_bits(std::uint32_t observed, std::uint32_t value,
                                         std::uint32_t owned, std::uint32_t transient) noexcept {
    return (observed & ~owned & ~transient) | (value & owned);
}
} // namespace psx::hwio
