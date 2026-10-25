#pragma once
#include "snapshot.hpp"

namespace psx::fmc {
using Hash=psx::situation::Hash;
inline constexpr std::uint16_t schema=2;
inline constexpr std::size_t header_bytes=256,max_nodes=65536,max_cells=262144;
inline constexpr std::uint32_t external_reference=0x80000000u;
enum class Model:std::uint32_t {legacy=1,ng=2};
enum class Kind:std::uint32_t {
 record=1,boolean_array=2,byte_array=3,char_array=4,short_array=5,
 int_array=6,long_array=7,float_array=8,double_array=9,reference_array=10,
 string=11,string_builder=12
};
enum class Scalar:std::uint32_t {boolean=1,byte=2,character=3,short_integer=4,
 integer=5,long_integer=6,real32=7,real64=8,reference=9};
struct Cell {
 std::uint32_t field{};Scalar kind{Scalar::integer};std::uint64_t bits{};
 bool operator==(const Cell&)const=default;
};
// Object identity is a bounded artifact ID, never a JVM pointer. External
// references name audited live bindings; their contents are not serialized.
struct Node {
 std::uint32_t id{},type{};Kind kind{Kind::record};
 std::vector<Cell> cells;std::vector<std::uint8_t> bytes;
 bool operator==(const Node&)const=default;
};
struct Root {
 std::uint32_t role{},node{};bool operator==(const Root&)const=default;
};
enum ContextValidity:std::uint32_t {reference=1,altitude=2,utc=4,pause=8};
struct Context {
 std::uint32_t validity{},pause_mask{};
 // Context is an independently timed diagnostic observation, not a claim of
 // simultaneous whole-aircraft checkpointing. Angles radians, MSL feet, UTC ms.
 std::int64_t observation_qpc{},utc_milliseconds{};
 double latitude_radians{},longitude_radians{},altitude_msl_feet{};
 bool operator==(const Context&)const=default;
};
struct Snapshot {
 std::string source_version;
 Model model{Model::ng};bool complete{};
 Hash source_jar{},profile{},navigation_database{},model_configuration{},parent{},payload_hash{};
 std::uint32_t source_pid{};std::uint64_t process_birth{},lifecycle{},generation{},frequency{},producer{},sequence{},native_frame{};
 std::int64_t observation_qpc{},copy_finish_qpc{};
 Context context;std::vector<Root> roots;std::vector<Node> nodes;
};
struct Compatibility {
 Hash source_jar{},profile{},navigation_database{},model_configuration{};Model model{Model::ng};
};
// These are structural/compatibility checks on workers. Native field coverage,
// roles, capacities and allowed values must additionally match the installed
// qualified profile before an import can become a mutation.
void validate(const Snapshot&);
void require_compatible(const Snapshot&,const Compatibility&);
std::vector<std::uint8_t> encode(const Snapshot&);
Snapshot inspect(std::span<const std::uint8_t>);
std::uint16_t artifact_schema(std::span<const std::uint8_t>)noexcept;
}
