#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hwio::flightillusion {
using Command = std::array<std::uint8_t,6>;
using Reply = std::array<std::uint8_t,25>;
std::optional<Command> encode(unsigned address,unsigned command,int value) noexcept;
Command identify(unsigned address); // Vendor's special 7C/80/AA identification request.
class Framer {
    Reply bytes_{};std::size_t size_{};
public:
    std::vector<Reply> feed(std::span<const std::uint8_t>);
    void reset() noexcept {size_=0;}
};
struct Port {std::string name,friendly,instance,path;bool operator==(const Port&)const=default;};
std::vector<Port> enumerate_ports();
enum class Profile {unassigned,altimeter,attitude,airspeed,brake,compass,clock_captain,clock_fo};
std::string profile_name(Profile);
std::string hardware_name(unsigned type);
bool compatible(Profile,unsigned type) noexcept;
struct Capabilities {
    bool needle_calibration{},firmware_altitude{},barometer_input{},clock_calendar{},clock_time_setting{},clock_buttons{};
};
Capabilities capabilities(unsigned type) noexcept;
struct Channel {std::string name,unit;double minimum{},maximum{};int native_min{},native_max{};unsigned command{};bool cyclic{},firmware{};};
std::vector<Channel> channels(Profile);
struct Point {double value{};int position{};bool captured{};bool operator==(const Point&)const=default;};
enum class ClockSource {psx_utc,local_utc,local_pc};
std::string clock_source_name(ClockSource);
// Windows' current time-zone/DST rules applied to a single sampled UTC epoch.
// The result encodes local calendar fields for the hardware, not a UTC instant.
std::optional<std::int64_t> local_pc_epoch(std::int64_t utc_epoch_ms) noexcept;
struct Calibration {std::vector<Point> points;bool operator==(const Calibration&)const=default;};
Calibration initial_calibration(const Channel&);
bool valid_calibration(const Channel&,const Calibration&,std::string&);
std::optional<int> position(const Channel&,const Calibration&,double) noexcept;
struct Assignment {
    std::string bus;unsigned address{},type{},model{},firmware{};Profile profile{};
    std::vector<Calibration> calibration;
    ClockSource clock_source{ClockSource::local_utc};
    bool operator==(const Assignment&)const=default;
};
struct Configuration {Port port;std::vector<Assignment> assignments,drafts;bool operator==(const Configuration&)const=default;};
bool valid_configuration(const Configuration&,std::string&);
std::string configuration_text(const Configuration&);
bool parse_configuration(std::string_view,Configuration&,std::string&);
class Store {
public:
    virtual ~Store()=default;
    virtual bool load(Configuration&,std::string&)=0;
    virtual bool save(const Configuration&,std::string&)=0;
};
std::shared_ptr<Store> file_store(std::filesystem::path);
class Link {
public:
    virtual ~Link()=default;
    virtual bool open(const Port&,std::string&)=0;
    // Nonblocking partial I/O. -1 is a transport fault; 0 means no progress.
    virtual int read(std::span<std::uint8_t>,std::string&)=0;
    virtual int write(std::span<const std::uint8_t>,std::string&)=0;
    virtual void close() noexcept=0;
};
struct Driver {std::function<std::vector<Port>()> enumerate;std::function<std::unique_ptr<Link>()> create;};
Driver native_driver();
enum class Signal:unsigned {altitude,bank,pitch,limit_speed,ias,brake,compass,count};
enum class ClockSync {normal,unavailable,paused,rate,setting,display_off};
// GMT/date only. Elapsed time and chronograph are separate aircraft functions;
// the measured type-104 protocol does not yet expose their complete controls.
struct Clock {std::int64_t epoch_ms{};bool valid{};ClockSync sync{ClockSync::normal};};
struct Sources {
    // One host producer. sample_ms uses steady_clock milliseconds (or the fixture
    // pump's clock). A new serial connection needs a newly published snapshot.
    // Data older than 2 seconds is unavailable; a retained needle is not a valid reading.
    std::uint64_t session{},sequence{},sample_ms{};
    // Feet, degrees (bank positive left), knots and psi. Absence is not zero.
    std::array<std::optional<double>,unsigned(Signal::count)> values;
    // Optional stricter source expiry in the same monotonic clock. Zero uses
    // the whole-snapshot watchdog. In particular, exporter limits expire <1 s.
    std::array<std::uint64_t,unsigned(Signal::count)> valid_until_ms{};
    std::array<std::string,unsigned(Signal::count)> unavailable;
    std::array<Clock,2> clocks;
    Clock pc_utc,pc_local;
    bool controls_ready{true};
};
enum class InputKind {barometer_inhg,barometer_delta_inhg,clock_epoch_ms};
// session is the bus connection; generation changes when that instrument returns
// or changes identity. Events contain observed changes, never PSX commands.
struct InputEvent {std::uint64_t session{},sequence{},generation{};unsigned address{};Profile profile{};InputKind kind{};double value{};std::int64_t epoch_ms{};std::uint64_t sample_ms{};};
struct Instrument {
    unsigned address{},type{},model{},firmware{};std::uint64_t generation{},seen_ms{};
    std::string name,status,input_status,calibration_status;
    bool connected{},compatible{},calibrating{};
    Profile assigned{};std::vector<std::optional<int>> commanded;
};
struct CalibrationEdit {unsigned address{},needle{};Profile profile{};std::vector<Calibration> calibration;std::optional<int> commanded,requested;bool available{true};};
struct View {
    Configuration configuration;std::vector<Port> ports;std::vector<Instrument> instruments;
    std::optional<CalibrationEdit> calibration;
    std::string status;std::uint64_t session{},revision{},sent{},malformed{},input_overflows{};
    bool connected{},connecting{},searching{};unsigned searched{};
};
// Independent provider: no HWIO Runtime, Mapper, Main TCP or process-global clock state.
// The deterministic pump is used only with start_worker=false in authored fixtures.
class Provider {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit Provider(std::shared_ptr<Store>,Driver={},bool start_worker=true);
    ~Provider();Provider(const Provider&)=delete;Provider& operator=(const Provider&)=delete;
    void stop();
    void refresh_ports();
    bool select_port(const Port&,std::string&);
    bool connect(std::string&);void disconnect();
    bool search(std::string&);void cancel_search();
    bool assign(unsigned,Profile,std::string&);
    bool select_clock_source(unsigned,ClockSource,std::string&);
    bool begin_calibration(unsigned,std::string&);
    bool select_needle(unsigned,std::string&);
    bool move(int,std::string&);
    bool capture(double,std::string&);
    bool capture_endpoint(bool maximum,double,std::string&);
    bool remove_point(double,std::string&);
    bool save_calibration(std::string&);void cancel_calibration();
    void publish(const Sources&);
    std::vector<InputEvent> drain_inputs();
    View snapshot()const;
    void pump(std::uint64_t monotonic_ms);
};
} // namespace hwio::flightillusion
