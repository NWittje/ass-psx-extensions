#pragma once
#include "fmc.hpp"

namespace psx::fmc {
struct FieldRule {
 std::uint32_t id{};Scalar kind{Scalar::integer};bool nullable{};
 std::vector<std::uint32_t> owned_types,external_roles;
};
struct TypeRule {
 std::uint32_t id{};Kind kind{Kind::record};
 std::uint32_t minimum_elements{},maximum_elements{};
 std::vector<FieldRule> fields;
 // Reference arrays have one component rule with id1. Record fields are exact:
 // omission is not permission to leave part of native memory unchanged.
 FieldRule component;
};
struct RootRule {std::uint32_t role{},type{};};
struct Profile {
 Compatibility compatibility;
 bool capture_qualified{},recall_qualified{};
 std::vector<TypeRule> types;std::vector<RootRule> roots;
};
// Worker-only checks. A Profile comes from installed bytecode qualification,
// never from the imported artifact. Structural validity alone grants no writes.
void validate_profile(const Profile&);
void require_profile(const Snapshot&,const Profile&,bool for_recall);
}
