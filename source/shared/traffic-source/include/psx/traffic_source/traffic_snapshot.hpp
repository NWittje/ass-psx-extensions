#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace psx::traffic {
constexpr std::uint32_t abi=1,slot_count=7,anchor_revision=1;
struct Header {
 std::uint32_t abi,size,slots,anchor;
 std::uint64_t sequence,lifecycle,frame,observed,finished,writer_version;
 std::uint32_t flags;std::int32_t mode;std::uint32_t process,producer;
 std::uint64_t process_created,frequency,reserved_identity;
 std::int64_t native_delta;double rate;std::uint32_t metadata_valid,reserved;
};
struct Slot {
 std::uint64_t generation;std::int32_t id;std::uint32_t valid,flags,reserved;
 std::uint64_t reserved_identity;
 // lat/lon radians, feet, true track/pitch/positive-right bank radians,
 // knots, actual/requested ft/min, track rate radians/native millisecond.
 std::array<double,10> values;
};
struct Snapshot {Header header;std::array<Slot,slot_count> slots;};
static_assert(sizeof(Header)==128&&sizeof(Slot)==112&&sizeof(Snapshot)==912);
static_assert(offsetof(Header,native_delta)==104&&offsetof(Slot,values)==32);
}
