#pragma once
#include <cstdint>
#include <cstring>
#include <array>
namespace psx::display {
inline constexpr std::uint32_t magic=0x314f5044;
inline constexpr std::size_t maximum=65520,snapshot_bytes=2048,record_bytes=1024,queue_capacity=8;
struct Header {std::uint32_t magic_value=magic;std::uint16_t version=1,kind{};std::uint32_t bytes{},sequence{};};
static_assert(sizeof(Header)==16);
inline std::uint64_t word(const void* bytes,std::size_t offset) noexcept {std::uint64_t v{};std::memcpy(&v,static_cast<const std::uint8_t*>(bytes)+offset,8);return v;}
inline void word(void* bytes,std::size_t offset,std::uint64_t value) noexcept {std::memcpy(static_cast<std::uint8_t*>(bytes)+offset,&value,8);}
struct Batch {std::uint32_t size{};std::array<std::uint8_t,maximum> bytes{};};
inline bool valid(const void* bytes,std::size_t length) noexcept {
    if(!bytes||length<snapshot_bytes||length>maximum||word(bytes,0)!=1||word(bytes,8)!=record_bytes)return false;
    auto n=word(bytes,128);return n<=61&&length==snapshot_bytes+n*record_bytes;
}
}
