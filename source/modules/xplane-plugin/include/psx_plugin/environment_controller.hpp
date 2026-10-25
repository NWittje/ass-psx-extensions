#pragma once
#include "environment_types.hpp"
#include "sdk_api.hpp"
#include <XPLMUtilities.h>
#include <XPLMWeather.h>
namespace psx::xp {
class EnvironmentController final {
public:
    bool begin(SdkApi&,motion::InstanceId,std::uint64_t,std::int64_t (*)()) noexcept;
    void retire() noexcept;
    void connection(std::uint64_t) noexcept;
    void boundary() noexcept; // Actual airport/situation/reposition completion, never tile streaming.
    env::Ack command(const EnvironmentCommand&) noexcept;
    void sample(const EnvironmentSample&) noexcept;
    void tick(bool connected) noexcept; // SDK pre-flight only; weather target/readback at most1Hz.
    const std::array<env::Applied,2>& applied() const noexcept {return reports_;}
    const std::array<envp::VisibilityPolicy,2>& visibility_policies() const noexcept {return policies_;}
    envp::AppliedPresentation weather_report() const noexcept;
    void capture(std::uint64_t generation) noexcept;
    bool timing(EnvironmentTiming&) noexcept;
    std::uint32_t owned() const noexcept {return owned_;}
    enum class FailureStage:unsigned {binding=0,read,baseline,setter_read,setter_mismatch,array_before,array_after,array_mismatch,pressure_anchor,profile_height,profile_order,qnh_reference,owner_read,owner_mismatch};
    struct Failure {
        unsigned binding{},index{};FailureStage stage{};Status status{};
        std::uint32_t requested_bits{},actual_bits{},expected_type{},actual_type{},expected_length{},actual_length{};
        std::uint64_t source_sequence{},connection{};
    };
    // SDK-owner-only fixed queue. Each binding/index/stage is recorded once per connection;
    // changing source values cannot create a per-sample trace. Journal drains are bounded.
    bool failure(Failure&) noexcept;
    std::uint64_t failures_lost() const noexcept {return failures_lost_;}
private:
    enum Id:unsigned {system_time,date,time,change,immediate,cloud_type,coverage,base,top,
        wind_speed,wind_direction,temperatures,sea_temperature,qnh,visibility,rain,levels,
        latitude,longitude,altitude,actual_wind_speed,actual_wind_direction,actual_temperature,actual_qnh,actual_visibility,
        actual_coverage,actual_base,actual_top,actual_pressure,pressure_altitude,count};
    struct Binding {XPLMDataRef ref{};XPLMDataTypeID type{};unsigned length{};bool writable{};unsigned actual_type{},actual_length{};Status status{};};
    struct Saved {std::array<float,13> values{},written{},target{};std::array<std::uint64_t,13> submitted_callback{};
        std::uint16_t mask{},written_mask{},uncertain_mask{},target_mask{},pending_mask{},delayed_mask{},restoring_mask{};};
    struct Claim {EnvironmentCommand command{};EnvironmentSample input{};std::int64_t expires{};bool pending{},have_sample{};};
    SdkApi* sdk_{};motion::InstanceId provider_{};std::uint64_t frequency_{},connection_{},generation_{1};
    std::int64_t (*clock_)(){};std::int64_t floor_{},last_weather_{},last_readback_{},held_utc_{};
    bool settling_{};std::uint32_t owned_{};
    std::uint64_t callback_{};
    std::uint64_t prepared_weather_target_{},reported_weather_target_{};
    struct TemperatureTarget {
        motion::InstanceId producer{};std::uint64_t owner{},identity{},lifecycle{},sequence{};
        std::array<float,13> profile{};double pressure_m{};float sea{};bool valid{};
    } temperature_target_;
    double prepared_pressure_m_{};
    EnvironmentTiming timing_{},gap_timing_{};bool have_gap_timing_{};
    std::int64_t last_timing_publish_{},last_timing_tick_{};
    struct TimingScope {
        EnvironmentController& owner;EnvironmentStage stage;std::int64_t begin;unsigned groups;
        TimingScope(EnvironmentController& o,EnvironmentStage s,unsigned g=0) noexcept:owner(o),stage(s),begin(o.timing_.generation?o.clock_():0),groups(g?g:(static_cast<unsigned>(s)<3?1u:2u)){}
        ~TimingScope(){if(begin)owner.trace(stage,begin,groups);}
    };
    void trace(EnvironmentStage,std::int64_t,unsigned) noexcept;
    std::int64_t settling_begin_{},settling_end_{};
    std::array<Binding,count> bindings_{};std::array<Saved,count> saved_{};
    std::array<Claim,2> claims_{};std::array<env::Applied,2> reports_{};
    std::array<env::Reason,2> release_reasons_{};
    std::array<envp::VisibilityPolicy,2> policies_{};
    using WeatherGetter=int (*)(double,double,double,XPLMWeatherInfo_t*);
    WeatherGetter weather_getter_{};
    std::array<Failure,128> failures_{};unsigned failure_head_{},failure_size_{};
    std::array<std::array<std::uint16_t,13>,count> failure_seen_{};
    std::uint64_t failures_lost_{};
    void fail(Id,unsigned,FailureStage,Status,float=0,float=0) noexcept;
    bool read(Id,unsigned,float&,bool=false,Status* =nullptr) noexcept;
    bool restoration_pending() const noexcept;
    bool write(Id,unsigned,float,bool force=false) noexcept;
    bool write_array(Id,const std::array<float,13>&,bool* =nullptr,std::uint16_t force=0) noexcept;
    bool submit(Id,unsigned,float) noexcept;
    bool submit_array(Id,const std::array<float,13>&) noexcept;
    bool confirm(Id,unsigned,float) noexcept;
    void retry_restoration() noexcept;
    bool save(Id,unsigned) noexcept;
    bool restore(Id,unsigned) noexcept;
    env::Reason ownership_issue(unsigned) noexcept;
    void release(unsigned,env::Reason) noexcept;
    void apply_time(std::int64_t,bool) noexcept;
    void apply_weather(std::int64_t) noexcept;
    void confirm_weather() noexcept;
    void readback(std::int64_t) noexcept;
};
}
