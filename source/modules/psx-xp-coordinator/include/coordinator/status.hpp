#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace coordinator {
enum class State { idle, starting, listening, connecting, connected, disconnected, error, stopping, stopped };
enum class Channel { tcp, pipe };
enum class Level { info, warning, error };
const char* state_name(State state) noexcept;
std::int64_t qpc_now() noexcept;
std::int64_t qpc_frequency() noexcept;

struct EndpointStatus {
    State state = State::idle;
    std::uint64_t epoch = 0, bytes_in = 0, bytes_out = 0, frames = 0, errors = 0, truncated = 0;
    std::uint32_t last_error = 0;
    std::int64_t last_frame_qpc = 0;
    std::array<char, 192> endpoint{};
};
struct StatusSnapshot { EndpointStatus tcp, pipe; };
struct Event {
    std::uint64_t sequence = 0;
    std::int64_t qpc = 0;
    Level level = Level::info;
    std::array<char, 192> text{};
};
class EventLog {
public:
    static constexpr std::size_t capacity = 256;
    void add(Level level, std::string_view message);
    std::vector<Event> snapshot() const;
    std::uint64_t overwritten() const;
private:
    mutable std::mutex mutex_;
    std::array<Event, capacity> events_{};
    std::uint64_t next_ = 1;
};
class StatusStore {
public:
    StatusSnapshot snapshot() const;
    void state(Channel channel, State state, std::uint32_t error = 0);
    void endpoint(Channel channel, std::string_view text);
    void new_epoch(Channel channel);
    void received(Channel channel, std::size_t bytes);
    void sent(Channel channel, std::size_t bytes);
    void frame(Channel channel);
    void truncated(Channel channel);
private:
    EndpointStatus& get(Channel channel) noexcept;
    mutable std::mutex mutex_;
    StatusSnapshot value_{};
};
}
