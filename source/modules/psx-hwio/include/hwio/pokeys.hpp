#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <psx/hwio/contract.hpp>

// Native protocol: PoLabs specification 14.3.2025. Byte offsets below are
// zero-based; the manufacturer manual uses one-based byte numbers.
namespace hwio::pokeys {
using Packet = std::array<std::uint8_t, 64>;
constexpr std::uint16_t port = 20055;
using Channel = psx::hwio::ResourceKind; // Compatibility spelling for the PoKeys packet adapter.
struct Identity {
    std::uint32_t serial{};
    std::uint8_t hardware{}, major{}, minor{}, patch{}, firmware_type{};
    bool locked{}, bootloader{};
    std::string name, build;
    bool supported() const noexcept;
    std::string key() const;
};
struct Capabilities {
    unsigned digital{}, analog{}, encoders{}, counters{}, matrix_keys{}, pwm{}, led{}, lcd{}, poext{}, ponet{}, sensors{};
    bool i2c{}, spi{}, onewire{}, poil{}, ppm{}, motion{}, can{}, uart{};
    bool onewire_fixed_pin{};
};
Capabilities capabilities(const Identity&) noexcept;
// PoLabs model capability tables, physical pin numbers (1..55). Not 0x1F telemetry.
std::uint8_t pin_capabilities(unsigned physical_pin) noexcept;
Packet request(std::uint8_t command, std::uint8_t option = 0, std::uint8_t argument = 0) noexcept;
void seal(Packet&, std::uint8_t request_id) noexcept;
bool runtime_request(const Packet&) noexcept; // Readback or output operation; excludes board setup/flash writes.
bool valid_reply(std::span<const std::uint8_t>, const Packet&) noexcept;
bool identity(const Packet&, Identity&);
std::uint16_t be16(const Packet&, std::size_t) noexcept;
std::uint32_t le32(const Packet&, std::size_t) noexcept;
void put32(Packet&, std::size_t, std::uint32_t) noexcept;

struct Pin { // Native readback. Ownership fields exist only to consume legacy saved setup.
    bool owned{};
    std::uint8_t mode{}, counter_options{};
};
struct Encoder {
    bool owned{}, enabled{};
    std::uint8_t a{}, b{}, options{1}; // physical pins are 1-based in configuration.
    std::uint16_t counts_per_detent{4};
};
struct Matrix {
    bool owned{}, enabled{};
    std::uint8_t rows{}, columns{}, decimation{};
    std::array<std::uint8_t, 16> row_pins{};
    std::array<std::uint8_t, 8> column_pins{};
};
struct Pwm {
    std::uint8_t owned{}, enabled{};
    std::uint32_t period{25000};
};
struct Display {
    bool owned{}, enabled{};
    std::uint8_t rows{4}, columns{20}, pins{1};
};
struct Motion {
    bool owned{}, enabled{}, require_home{true};
    unsigned axis{};
    std::string unit;
    double steps_per_unit{1}, minimum{}, maximum{}, maximum_speed{}, acceleration{};
    std::uint8_t step_pin{}, direction_pin{}, home_pin{}, negative_limit{}, positive_limit{};
};
struct Peripheral {
    Channel kind{Channel::sensor};
    std::uint16_t channel{}, address{}, length{1};
    std::uint32_t rate{1}, options{};
    bool enabled{};
    std::vector<std::uint8_t> pins;
    std::vector<std::uint8_t> data; // Sensor ID or explicitly configured bus payload.
};
// Readback model plus legacy decoder fields. No controller setup writer exists.
struct Configuration {
    std::array<Pin,55> pins{};
    std::array<Encoder,25> encoders{};
    Encoder ultra;
    std::uint8_t fast_configuration{},fast_options{};
    Matrix matrix;
    Pwm pwm;
    Display lcd;
    std::array<Display,2> led{{{false,false,8,8,1},{false,false,8,8,1}}};
    std::vector<Peripheral> peripherals;
    std::vector<Motion> motion;
    std::uint16_t watchdog_ms{500};
};
std::array<std::string,55> pin_functions(const Configuration&);
Packet digital_output(std::uint64_t owned, std::uint64_t values) noexcept;
Packet pwm_output(const Packet& observed, std::uint8_t owned,
                  const std::array<std::uint32_t,6>& values) noexcept;
Packet led_pixel(unsigned display, unsigned row, unsigned column, bool on) noexcept;
Packet poil_read(unsigned first,unsigned count) noexcept;
Packet poil_write(unsigned slot,std::int32_t value) noexcept;
bool motion_reply(const Packet&,const Packet&) noexcept;
// A cycle is measured from its first request to its final reply, excluding the
// scheduled idle interval. No allocator, sorting or logging runs per sample.
class Rate {
    std::array<double,512> samples_{};
    std::size_t next_{}, count_{};
    double period_ms_{4}, next_adjust_ms_{};
    std::uint64_t overruns_{};
public:
    void reset() noexcept;
    void cycle(double service_ms, double now_ms, double fixed_hz = 0) noexcept;
    void fault() noexcept;
    double period_ms() const noexcept { return period_ms_; }
    std::uint64_t overruns() const noexcept { return overruns_; }
};
std::int64_t encoder_delta(std::uint32_t previous, std::uint32_t current) noexcept;
} // namespace hwio::pokeys
