#include "coordinator/status.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace coordinator {
namespace {
template<std::size_t N> void copy_text(std::array<char, N>& to, std::string_view from) {
    const auto n = (std::min)(N - 1, from.size());
    if (n) std::memcpy(to.data(), from.data(), n);
    to[n] = '\0';
}
}
const char* state_name(State s) noexcept {
    switch (s) {
    case State::idle: return "Off"; case State::starting: return "Starting";
    case State::listening: return "Listening"; case State::connecting: return "Connecting";
    case State::connected: return "Connected (transport only)";
    case State::disconnected: return "Disconnected"; case State::error: return "Error";
    case State::stopping: return "Stopping"; case State::stopped: return "Stopped";
    } return "Unknown";
}
std::int64_t qpc_now() noexcept { LARGE_INTEGER v{}; QueryPerformanceCounter(&v); return v.QuadPart; }
std::int64_t qpc_frequency() noexcept { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return v.QuadPart; }
void EventLog::add(Level level, std::string_view text) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = events_[(next_ - 1) % capacity];
    e.sequence = next_++; e.qpc = qpc_now(); e.level = level; copy_text(e.text, text);
}
std::vector<Event> EventLog::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Event> result;
    auto count = (std::min)(next_ - 1, static_cast<std::uint64_t>(capacity));
    result.reserve(static_cast<std::size_t>(count));
    for (auto sequence = next_ - count; sequence < next_; ++sequence)
        result.push_back(events_[(sequence - 1) % capacity]);
    return result;
}
std::uint64_t EventLog::overwritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_ > capacity + 1 ? next_ - capacity - 1 : 0;
}
EndpointStatus& StatusStore::get(Channel c) noexcept { return c == Channel::tcp ? value_.tcp : value_.pipe; }
StatusSnapshot StatusStore::snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return value_; }
void StatusStore::state(Channel c, State s, std::uint32_t error) {
    std::lock_guard<std::mutex> lock(mutex_); auto& e = get(c); e.state = s;
    if (error) { e.last_error = error; ++e.errors; }
}
void StatusStore::endpoint(Channel c, std::string_view s) { std::lock_guard<std::mutex> lock(mutex_); copy_text(get(c).endpoint, s); }
void StatusStore::new_epoch(Channel c) { std::lock_guard<std::mutex> lock(mutex_); auto& e = get(c); ++e.epoch; e.last_frame_qpc = 0; e.last_error = 0; }
void StatusStore::received(Channel c, std::size_t n) { std::lock_guard<std::mutex> lock(mutex_); get(c).bytes_in += n; }
void StatusStore::sent(Channel c, std::size_t n) { std::lock_guard<std::mutex> lock(mutex_); get(c).bytes_out += n; }
void StatusStore::frame(Channel c) { std::lock_guard<std::mutex> lock(mutex_); auto& e = get(c); ++e.frames; e.last_frame_qpc = qpc_now(); }
void StatusStore::truncated(Channel c) { std::lock_guard<std::mutex> lock(mutex_); ++get(c).truncated; }
}
