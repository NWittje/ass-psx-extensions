#pragma once
#include "navigation_selection.hpp"
#include <optional>
namespace psx::instructor {
struct FlapSpeeds {
  std::uint32_t pid{};
  std::uint64_t process_start_unix_ms{},sequence{},sampled_unix_ms{};
  double gross_weight_kg{};
  int calculation_weight_kg{},landing_altitude_ft{},ref25_kt{},ref30_kt{};
  // Native PFD UP manoeuvring marker in knots; historical schema1 has no marker.
  std::optional<int> up_maneuver_kt;
  std::string up_notice;
};
class PerformanceSelection {
  std::filesystem::path directory_;
  std::optional<FlapSpeeds> last_;
  std::string last_bytes_;
public:
  explicit PerformanceSelection(std::filesystem::path directory = {});
  static FlapSpeeds parse(std::string_view,std::uint32_t,std::uint64_t,std::uint64_t now_unix_ms);
  FlapSpeeds read(const NavigationSelection &,std::uint64_t now_unix_ms);
};
}
