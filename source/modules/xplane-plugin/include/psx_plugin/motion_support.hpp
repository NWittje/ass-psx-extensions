#pragma once
#include <psx/bridge/protocol.hpp>
#include <atomic>
#include <algorithm>

namespace psx::xp {
// A required control cannot be replaced by newer continuous state. One writer
// and one reader; the owner retires both threads before destroying this queue.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324)
#endif
template<class T,std::size_t N> class SpscFifo {
public:
    bool empty() const noexcept {return tail_.load(std::memory_order_relaxed)==head_.load(std::memory_order_acquire);}
    bool push(const T& value) noexcept {
        const auto h=head_.load(std::memory_order_relaxed);
        if(h-tail_.load(std::memory_order_acquire)>=N)return false;
        data_[h%N]=value;head_.store(h+1,std::memory_order_release);return true;
    }
    bool pop(T& value) noexcept {
        const auto t=tail_.load(std::memory_order_relaxed);
        if(t==head_.load(std::memory_order_acquire))return false;
        value=data_[t%N];tail_.store(t+1,std::memory_order_release);return true;
    }
private:
    std::array<T,N> data_{};
    alignas(64) std::atomic<std::uint64_t> head_{};
    alignas(64) std::atomic<std::uint64_t> tail_{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
struct MotionControl {
    bool boundary{};
    bool source_selection{};
    bridge::MotionRequest request{};
    bridge::SessionStatus session{};
    bridge::SourceSelection selection{};
};
struct MotionSamples {
    std::array<std::uint64_t,128> ticks{};
    std::size_t count{},next{};
    void add(std::uint64_t t) noexcept {ticks[next]=t;next=(next+1)%ticks.size();count=(std::min)(count+1,ticks.size());}
    bridge::Distribution distribution() const noexcept { // Worker only: sorting never belongs in a callback.
        if(!count)return {};auto values=ticks;std::sort(values.begin(),values.begin()+count);
        return {count,values[0],values[count-1],values[(count*50+99)/100-1],values[(count*95+99)/100-1],values[(count*99+99)/100-1]};
    }
};
struct MotionReport {
    bridge::MotionStatus status{};
    MotionSamples pre{},post{},age{},pitch{},interval{},output_age{},hold_duration{},exporter_age{},sampling_width{};
};
struct ProfileRequest {std::uint64_t generation{};std::array<char,512> path{};};
struct NominalFootprint {
    bool qualified{};
    motion::InstanceId id{};
    std::uint32_t revision{};
    // Fully extended strut-end assembly centres, metres right/up/aft from original CG.
    // Nose/bodyR/bodyL/wingR/wingL; no tire contact, load or PSX compression inference.
    std::array<motion::Vec3,5> centres{};
};
struct MotionProfile {
    std::uint64_t generation{};
    bool qualified{};
    bool sparky{},audio_qualified{},aircraft_state_qualified{};
    motion::Vec3 eye{};
    double cg_y_ft{},cg_z_ft{};
    std::array<char,65> sha256{};
    NominalFootprint footprint{};
};
// Off SDK callback; finite 8 MiB file limit and exact installed/generated ACF identity.
MotionProfile verify_motion_profile(const ProfileRequest&) noexcept;
struct MotionTimeout {std::uint32_t milliseconds{motion::default_boost_timeout_ms};bool valid{true},provided{};};
MotionTimeout parse_motion_timeout(std::wstring_view) noexcept;
MotionTimeout motion_timeout_from_environment() noexcept; // Enable only; process environment, no files/registry.
}
