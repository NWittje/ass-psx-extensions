#pragma once
#include <psx/motion/full_pose.hpp>
#include <psx/audio/audio.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::exporter {
inline constexpr std::uint16_t protocol_version=2;
inline constexpr std::size_t header_size=16,max_payload=4096,max_frame=header_size+max_payload;
inline constexpr char pipe_prefix[]="\\\\.\\pipe\\psx-ios.exporter.v1."; // SID.pid.creation-FILETIME
enum class Kind : std::uint16_t {hello=1,subscribe=2,snapshot=3,health=4,audio_snapshot=5,audio_lease=6,audio_ack=7};
struct Header {Kind kind{Kind::hello};std::uint32_t payload_size{},sequence{};std::uint16_t version{protocol_version};};
struct Hello {
    motion::InstanceId producer{};
    std::uint32_t pid{};
    std::uint64_t process_creation_filetime{},qpc_frequency{};
    std::uint32_t schema_revision{motion::exporter_schema_revision};
    std::array<char,64> build{};
    std::uint32_t audio_groups{},suppression_groups{};
};
struct Subscribe {motion::InstanceId producer{};std::uint64_t coordinator_instance{},qpc_frequency{};};
using Snapshot=motion::RawPose;
using AudioSnapshot=audio::Snapshot;
using AudioLease=audio::Lease;
using AudioAck=audio::LeaseAck;
// Opaque producer flags are diagnostic; Health alone never grants pose validity.
struct Health {
    std::uint64_t attempted{},rejected{},pose_epoch{};
    std::uint32_t flags{};
    std::uint64_t enqueued{},sent{},queue_dropped{},transport_dropped{};
    // Optional v2 tail; old v1 health records decode with zero audio counters.
    std::uint64_t audio_enqueued{},audio_sent{},audio_dropped{};
    std::uint32_t audio_groups{},suppression_groups{};
};
struct Frame {std::array<std::uint8_t,max_frame> bytes{};std::size_t size{};};
bool read_header(const std::uint8_t*,std::size_t,Header&) noexcept;
bool encode(const Hello&,std::uint32_t,Frame&) noexcept;
bool encode(const Subscribe&,std::uint32_t,Frame&) noexcept;
bool encode(const Snapshot&,std::uint32_t,Frame&) noexcept;
bool encode(const Health&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Hello&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Subscribe&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Snapshot&) noexcept;
bool decode(const std::uint8_t*,std::size_t,Health&) noexcept;
bool encode(const AudioSnapshot&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioSnapshot&) noexcept;
bool encode(const AudioLease&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioLease&) noexcept;
bool encode(const AudioAck&,std::uint32_t,Frame&) noexcept;
bool decode(const std::uint8_t*,std::size_t,AudioAck&) noexcept;
// v1 producer handshake compatibility: only Subscribe can be emitted as v1.
bool encode_legacy_subscribe(const Subscribe&,std::uint32_t,Frame&) noexcept;
using FrameHandler=bool (*)(void*,const Header&,const std::uint8_t*,std::size_t);
// One stream owner. Sticky failure; no scanning or resynchronization of bad input.
class Framer {
public:
    bool feed(const void*,std::size_t,FrameHandler,void*) noexcept;
    void reset() noexcept {used_=0;needed_=header_size;failed_=false;}
    bool failed() const noexcept {return failed_;}
    std::size_t buffered() const noexcept {return used_;}
private:
    std::array<std::uint8_t,max_frame> buffer_{};
    std::size_t used_{},needed_{header_size};bool failed_{};
};
} // namespace psx::exporter
