#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::sync {
inline constexpr std::uint16_t version=2;
inline constexpr std::size_t lane_count=8, header_size=104, max_object=8192,
    max_payload=max_object-header_size, ordered_capacity=16;
inline constexpr std::uint16_t coordinator_port=47520,plugin_port=47540;
inline constexpr std::uint32_t heartbeat_ms=250,connection_timeout_ms=1000,
    reconnect_ms=1000,clock_interval_ms=100;
enum class Role:std::uint16_t {coordinator=1,plugin=2};
enum class Lane:std::uint16_t {control,pose,terrain,audio,appearance,environment,traffic,status};
enum class Family:std::uint16_t {link,observation,terrain,aircraft,environment,status,traffic,actor};
enum class Kind:std::uint16_t {hello=1,attach,frame,heartbeat,ack,clock_request,clock_reply,detach};
enum Flag:std::uint16_t {ordered=1,replaceable=2};
struct Identity {
    std::uint64_t instance_high{},instance_low{},process_birth{};
    std::uint32_t pid{};
    bool operator==(const Identity&) const=default;
};
struct Header {
    Kind kind{Kind::hello};Lane lane{Lane::control};Family family{Family::link};
    std::uint16_t flags{};std::uint32_t bytes{};
    std::uint16_t wire_version{version};
    std::uint64_t connection{},sequence{};Identity producer;
    // mapping_generation is retained only for exact v1 Capture decoding.
    // v2 uses that wire word for the two service incarnations below; clock
    // generations remain in the coherent endpoint/checkpoint context.
    std::uint64_t frequency{},observed_qpc{},mapping_generation{},key{},control_floor{};
    std::uint32_t source_service{},target_service{};
};
struct Frame {std::array<std::uint8_t,max_object> bytes{};std::size_t size{};};
struct Hello {
    Role role{Role::coordinator};std::uint16_t receive_port{coordinator_port};
    std::uint32_t families{0xfe};std::uint64_t boot{},frequency{};
    Identity identity;
};
struct ServiceAnnouncement {std::uint32_t incarnation{},echo{};bool open{};bool operator==(const ServiceAnnouncement&)const=default;};
using ServiceAnnouncements=std::array<ServiceAnnouncement,7>;
inline constexpr std::size_t service_announcement_bytes=7*12;
bool valid(const Identity&) noexcept;
bool encode(const Header&,std::span<const std::uint8_t>,Frame&) noexcept;
bool decode(std::span<const std::uint8_t>,Header&,std::span<const std::uint8_t>&) noexcept;
bool encode_hello(const Hello&,std::span<std::uint8_t>,std::size_t&) noexcept;
bool decode_hello(std::span<const std::uint8_t>,Hello&) noexcept;
bool encode_services(const ServiceAnnouncements&,std::span<std::uint8_t>) noexcept;
bool decode_services(std::span<const std::uint8_t>,ServiceAnnouncements&) noexcept;
Lane classify(Family,std::span<const std::uint8_t>,bool& ordered,std::uint64_t& key) noexcept;
std::uint32_t period_ms(Lane,std::uint64_t key=0) noexcept;
}
