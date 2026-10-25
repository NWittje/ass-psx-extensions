#pragma once
#include "snapshot.hpp"

namespace psx::fmc::native_records {
using Hash=psx::situation::Hash;
inline constexpr std::uint16_t schema=3;
inline constexpr std::size_t header_bytes=256,max_records=256,max_key=64,max_value=65536;
struct Compatibility {Hash jar{},profile{};};
struct Provenance {
 std::string version;std::uint32_t pid{};
 std::uint64_t birth{},lifecycle{},generation{},frequency{},producer{},sequence{},frame{};
 std::int64_t observation{},finish{};Hash parent{};
};
struct Context {
 std::uint32_t validity{},pause{};std::int64_t observation{},utc_milliseconds{};
 double latitude{},longitude{},altitude{};
};
struct Metadata {
 Hash jar{},profile{},navigation{},configuration{};std::uint32_t model{};
 Provenance source;Context context;
};
struct Key {std::string name;bool required{};};
struct Manifest {std::vector<Key> keys;};
struct Record {
 std::string key;std::vector<std::uint8_t> value;
 bool operator==(const Record&)const=default;
};
struct Snapshot {Metadata metadata;bool complete{};std::vector<Record> records;Hash payload_hash{};};
// A profile supplies native saved keys and required/optional status. The codec
// does not interpret values, select an aircraft model, or resolve navigation.
void validate(const Snapshot&,const Manifest&);
void require_compatible(const Snapshot&,const Manifest&,const Compatibility&);
std::vector<std::uint8_t> encode(const Snapshot&,const Manifest&);
Snapshot inspect(std::span<const std::uint8_t>,const Manifest&);
Snapshot extract(std::span<const std::uint8_t> verified_native_save,Hash expected_hash,const Metadata&,const Manifest&);
std::vector<std::pair<std::string,std::string>> decoded_values(const Snapshot&,const Manifest&);
std::uint16_t artifact_schema(std::span<const std::uint8_t>)noexcept;
}
