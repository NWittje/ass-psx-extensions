#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace psx::xp {
struct TrafficModel {
 bool qualified{};
 bool sdk_loadable{};
 // Absolute UTF-8 filesystem identities remain available for qualification.
 std::array<std::string,8> objects{};
 // Worker-precomputed, forward-slash paths relative to the X-System root.
 std::array<std::string,8> sdk_objects{};
 std::vector<std::string> refs;
 std::vector<float> defaults;
};
bool traffic_sdk_path(std::string_view path) noexcept;
// Worker only. No filesystem access; fail closed outside the exact SDK root.
bool prepare_traffic_load_paths(TrafficModel& model,const std::filesystem::path& root) noexcept;
// Worker only: installed-input hash qualification and derived text generation.
TrafficModel prepare_traffic_model(const std::filesystem::path& root,const std::filesystem::path& cache,bool runway_actor=false) noexcept;
}
