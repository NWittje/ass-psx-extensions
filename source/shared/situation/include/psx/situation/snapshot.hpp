#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace psx::situation {
inline constexpr std::size_t max_artifact=8*1024*1024, max_line=65536;
using Hash=std::array<std::uint8_t,32>;
Hash sha256(std::span<const std::uint8_t>);
std::string hex(Hash);
struct Snapshot {
 std::string version;
 // Native files use writeBytes/readLine. Preserve bytes, including unknown fields,
 // rather than silently recoding descriptions or packed native strings as UTF-8.
 std::map<std::string,std::string,std::less<>> fields;
 Hash hash{};
};
Snapshot inspect(std::span<const std::uint8_t>);
void validate_native(const Snapshot&);
struct Setting {
 std::string id,label,key,raw,display,unit,value;
 bool qualified{};
};
std::vector<Setting> cockpit_settings(const Snapshot&);
enum class Match {match,mismatch,unobserved};
struct Comparison {Setting expected; Match state{Match::unobserved};};
// Caller supplies actual physical observations; a PSX echo is not such evidence.
std::vector<Comparison> compare(const Snapshot&,const std::map<std::string,std::string,std::less<>>& physical);
struct FmcSnapshot {
 std::string source_version;
 Hash parent_hash{},source_jar_hash{},payload_hash{};
 std::map<std::string,std::string,std::less<>> fields;
};
FmcSnapshot extract_fmc(const Snapshot&,Hash source_jar_hash={});
std::vector<std::uint8_t> encode_fmc(const FmcSnapshot&);
FmcSnapshot decode_fmc(std::span<const std::uint8_t>);
bool fmc_field(std::string_view)noexcept;
}
