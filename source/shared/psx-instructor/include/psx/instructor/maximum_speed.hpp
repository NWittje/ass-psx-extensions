#pragma once
#include "navigation_selection.hpp"
#include <optional>

namespace psx::instructor {
struct MaximumSpeed {
  std::uint32_t pid{};
  std::uint64_t process_start_unix_ms{},sequence{},sampled_unix_ms{},lifecycle{};
  // Native smoothed PFD barber pole, indicated knots; never a derived MMO/TAS.
  std::optional<double> max_speed_kt;
  std::string notice;
};
class MaximumSpeedSelection {
  std::filesystem::path directory_;
  std::optional<MaximumSpeed> last_;
  std::string last_bytes_;
public:
  explicit MaximumSpeedSelection(std::filesystem::path directory = {});
  static MaximumSpeed parse(std::string_view,std::uint32_t pid,std::uint64_t birth,std::uint64_t now_unix_ms);
  // Use the Main-associated NavigationSelection, including its discovered cache root.
  MaximumSpeed read(const NavigationSelection &,std::uint64_t now_unix_ms);
};
}
