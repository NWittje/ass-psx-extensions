#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace coordinator {
class LineFramer {
public:
    static constexpr std::size_t max_line = 131072;
    using Sink = std::function<void(std::string_view)>;
    bool feed(const char* data, std::size_t size, const Sink& sink);
    bool complete() const noexcept { return used_ == 0 && !cr_ && !failed_; }
private:
    std::array<char, max_line> buffer_{};
    std::size_t used_ = 0;
    bool cr_ = false, failed_ = false;
};
namespace pipe_v1 {
constexpr std::size_t header_size = 16, max_payload = 4096;
enum class Kind : std::uint16_t { ping = 1, pong = 2 };
struct Message {
    Kind kind = Kind::ping;
    std::uint32_t sequence = 0;
    std::uint32_t size = 0;
    std::array<std::uint8_t, max_payload> payload{};
};
std::vector<std::uint8_t> encode(const Message& message);
class Decoder {
public:
    using Sink = std::function<bool(const Message&)>;
    bool feed(const std::uint8_t* data, std::size_t size, const Sink& sink);
    bool complete() const noexcept { return used_ == 0 && !failed_; }
private:
    std::array<std::uint8_t, header_size + max_payload> buffer_{};
    std::size_t used_ = 0, needed_ = header_size;
    bool failed_ = false;
};
}
}
