#include "coordinator/protocol.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace coordinator {
bool LineFramer::feed(const char* p, std::size_t n, const Sink& sink) {
    if (failed_) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (c == '\n') { sink({buffer_.data(), used_}); used_ = 0; cr_ = false; continue; }
        if (cr_) { failed_ = true; return false; }
        if (c == '\r') { cr_ = true; continue; }
        if ((c < 32 && c != '\t') || c > 126 || used_ == max_line) { failed_ = true; return false; }
        buffer_[used_++] = static_cast<char>(c);
    }
    return true;
}
namespace pipe_v1 {
namespace {
void put16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get32(const std::uint8_t* p) { return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
bool kind_valid(std::uint16_t k) { return k == 1 || k == 2; }
}
std::vector<std::uint8_t> encode(const Message& m) {
    if (m.size > max_payload || !kind_valid(static_cast<std::uint16_t>(m.kind))) throw std::invalid_argument("pipe message bounds/kind");
    std::vector<std::uint8_t> bytes(header_size + m.size);
    std::memcpy(bytes.data(), "PXDC", 4); put16(bytes.data() + 4, 1); put16(bytes.data() + 6, static_cast<std::uint16_t>(m.kind));
    put32(bytes.data() + 8, m.size); put32(bytes.data() + 12, m.sequence);
    std::memcpy(bytes.data() + header_size, m.payload.data(), m.size); return bytes;
}
bool Decoder::feed(const std::uint8_t* p, std::size_t n, const Sink& sink) {
    if (failed_) return false;
    while (n) {
        auto count = (std::min)(n, needed_ - used_);
        std::memcpy(buffer_.data() + used_, p, count); used_ += count; p += count; n -= count;
        if (used_ != needed_) continue;
        if (needed_ == header_size) {
            if (std::memcmp(buffer_.data(), "PXDC", 4) || get16(buffer_.data() + 4) != 1 ||
                !kind_valid(get16(buffer_.data() + 6)) || get32(buffer_.data() + 8) > max_payload) { failed_ = true; return false; }
            needed_ = header_size + get32(buffer_.data() + 8);
            if (used_ < needed_) continue;
        }
        Message m; m.kind = static_cast<Kind>(get16(buffer_.data() + 6)); m.size = get32(buffer_.data() + 8); m.sequence = get32(buffer_.data() + 12);
        std::memcpy(m.payload.data(), buffer_.data() + header_size, m.size);
        if (!sink(m)) { failed_ = true; return false; }
        used_ = 0; needed_ = header_size;
    } return true;
}
}
}
