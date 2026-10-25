#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace psx::diagnostics {
inline constexpr std::uint16_t schema_version=1;
inline constexpr std::size_t max_producers=8, queue_capacity=512, error_reserve=64, recent_capacity=1024;
enum class Severity : std::uint8_t { debug=0, info=1, warning=2, error=3, fatal=4 };
enum class Component : std::uint8_t { application=0, main=1, boost=2, bridge=3, plugin=4, sdk=5, decoder=6, capture=7, replay=8, journal=9, exporter=10, crash_dump=11, motion=12, audio=13, terrain=14 };
enum class ErrorDomain : std::uint8_t { none=0, win32=1, winsock=2, hresult=3, protocol=4, decoder=5, sdk=6, application=7 };
enum class Code : std::uint16_t {
    message=1, process_start=2, process_stop=3, startup_failed=4, configuration=5, operator_marker=6,
    enable=10, disable=11, aircraft_changed=12, scenery_changed=13, sdk_failure=14,
    connect_requested=20, connected=21, synchronized=22, disconnected=23, retry=24, io_failure=25,
    handshake_rejected=30, frame_rejected=31, observation_rejected=32, source_invalidated=33,
    callback_delayed=40, callback_resumed=41, callback_gap=42, metrics=43,
    capture_start=50, capture_cutoff=51, capture_complete=52, capture_incomplete=53, replay_failed=54,
    events_lost=60, persistence_failed=61, persistence_recovered=62, repeated=63, clock_anchor=64,
    export_start=70, export_complete=71, export_incomplete=72, crash_policy=80, shutdown_result=81,
    motion_requested=90, motion_state=91, motion_rejected=92, motion_boundary=93,
    motion_placement=94, motion_cleanup=95, motion_ownership=96, motion_reference=97,
    terrain_state=100, terrain_metrics=101
};
struct Context {
    std::uint64_t run_id{}, plugin_load_id{}, enable_id{}, connection_id{}, session_epoch{}, configuration_epoch{}, boost_generation{}, aircraft_generation{}, scenery_generation{};
};
struct Event {
    Context context{};
    std::uint64_t sequence{};
    std::int64_t qpc{};
    std::uint16_t producer{};
    Code code{Code::message};
    Severity severity{Severity::info};
    Component component{Component::application};
    ErrorDomain error_domain{ErrorDomain::none};
    std::uint32_t error{};
    std::array<std::uint64_t,6> values{};
    std::array<char,160> text{};
};
template<std::size_t N> void text(std::array<char,N>& destination,std::string_view source) noexcept {
    const auto n=source.size()<N-1?source.size():N-1;
    for(std::size_t i=0;i<n;++i) destination[i]=source[i];
    destination[n]=0;
}
inline bool valid(const Event& e) noexcept {
    const auto code=static_cast<unsigned>(e.code);
    return e.context.run_id && e.sequence && e.qpc>=0 && e.producer<max_producers &&
        static_cast<unsigned>(e.severity)<=4 && static_cast<unsigned>(e.component)<=14 &&
        static_cast<unsigned>(e.error_domain)<=7 && ((code>=1 && code<=6) || (code>=10 && code<=14) || (code>=20 && code<=25) ||
        (code>=30 && code<=33) || (code>=40 && code<=43) || (code>=50 && code<=54) || (code>=60 && code<=64) ||
        (code>=70 && code<=72) || (code>=80 && code<=81) || (code>=90 && code<=97) || (code>=100 && code<=101)) && e.text.back()==0;
}
struct Maximum {
    std::uint64_t count{}, ticks{}, callback_count{};
    std::int64_t qpc{}, previous_qpc{};
    void add(std::uint64_t value,std::uint64_t callback,std::int64_t at,std::int64_t previous=0) noexcept {
        ++count;if(count==1 || value>ticks){ticks=value;callback_count=callback;qpc=at;previous_qpc=previous;}
    }
};
enum class Freshness : std::uint8_t { unavailable, current, delayed };
inline Freshness freshness(bool available,std::int64_t now,std::int64_t stamp,std::uint64_t frequency,std::uint32_t limit_ms=1000) noexcept {
    if(!available || !frequency || stamp<=0 || now<stamp) return Freshness::unavailable;
    return static_cast<long double>(now-stamp)*1000.0L>static_cast<long double>(frequency)*limit_ms?Freshness::delayed:Freshness::current;
}
const char* name(Severity) noexcept;
const char* name(Component) noexcept;
const char* name(Code) noexcept;
} // namespace psx::diagnostics
