#pragma once
#include "protocol.hpp"
#include "fmc.hpp"

namespace psx::fmc::wire {
inline constexpr std::uint16_t revision=2;
inline constexpr std::uint16_t native_revision=3;
inline constexpr std::size_t request_prefix=112,result_prefix=416;
// Preserve the existing fixed transport buffer. The larger response prefix
// leaves a smaller maximum data chunk; files retain the same8MiB bound.
inline constexpr std::size_t chunk_bytes=psx::situation::chunk_bytes-(result_prefix-240);
enum class Command:std::uint32_t {status=0,capture_situation=1,stage=2,recall_situation=3,lookup=4,fetch=5,legacy_fmc_recall=6,heartbeat=7,capture=8,recall=9};
enum Capability:std::uint32_t {capture_memory=32,recall_memory=64,native_records=128};
enum Evidence:std::uint32_t {complete_checkpoint=16,restored_readback=32,unrelated_preserved=64,rollback_restored=128,native_initialized=256};
enum Validity:std::uint32_t {compatibility=1,memory_readback=2,preservation=4,native_completion=8};
enum class Reason:std::uint32_t {compatibility=12,profile=13,concurrent_edit=14,rollback=15};
inline constexpr auto rolled_back=static_cast<psx::situation::Phase>(7);
struct Request {
 Command command{Command::status};psx::situation::ProcessIdentity client{},target{};
 std::uint64_t session{},operation{},lifecycle{},frequency{},generation{};
 std::int64_t issued_qpc{},deadline_qpc{};
 std::uint32_t offset{},total{};std::vector<std::uint8_t> data;
 bool operator==(const Request&)const=default;
};
struct Result {
 psx::situation::Result common;
 std::uint64_t generation{};std::uint32_t entry_pause_mask{},validity{};
 std::uint32_t model{}; //0 unavailable,1 legacy,2 NG; valid with compatibility.
 // Profile/nav/config describe the currently qualified backend. State hashes
 // are completion evidence, not substitutes for a current native observation.
 Hash profile{},navigation_database{},model_configuration{},restored_state{},preserved_state{};
};
bool encode(const Request&,std::uint32_t,psx::situation::Frame&)noexcept;
bool encode(const Result&,std::uint32_t,psx::situation::Frame&)noexcept;
bool decode(std::span<const std::uint8_t>,Request&,std::uint32_t&)noexcept;
bool decode(std::span<const std::uint8_t>,Result&,std::uint32_t&)noexcept;
bool encode(const Request&,std::uint32_t,psx::situation::Frame&,std::uint16_t version)noexcept;
bool encode(const Result&,std::uint32_t,psx::situation::Frame&,std::uint16_t version)noexcept;
bool decode(std::span<const std::uint8_t>,Request&,std::uint32_t&,std::uint16_t version)noexcept;
bool decode(std::span<const std::uint8_t>,Result&,std::uint32_t&,std::uint16_t version)noexcept;
// Revision3 uses the same framing and commands for opaqueFMC3 records. Its
// native_initialized/native_completion evidence claims native return + Pause,
// never preservation, rollback, graph readback or angular-rate reset.
// Negotiation is the first authenticated status request on a new connection.
// Revision changes require a new connection; mutations are never replayed.
}
