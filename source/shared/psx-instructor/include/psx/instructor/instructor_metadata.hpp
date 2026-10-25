#pragma once
#include "navigation_selection.hpp"
namespace psx::instructor {
struct InstructorSample {
  std::uint32_t pid{};
  std::uint64_t process_start_unix_ms{}, sequence{}, sampled_unix_ms{}, lifecycle_epoch{};
  std::optional<bool> loading;
  std::optional<double> corrected_altitude_ft, tropopause_ft;
  std::string corrected_altitude_error, tropopause_error;
};
// Read-only shared worker metadata. Independent fields; no Vref dependency.
class InstructorMetadata {
  std::filesystem::path directory_;
  std::optional<InstructorSample> last_;
  std::string last_bytes_;
public:
  explicit InstructorMetadata(std::filesystem::path = {});
  static InstructorSample parse(std::string_view, std::uint32_t pid, std::uint64_t birth, std::uint64_t utc_ms);
  InstructorSample read(const NavigationSelection &, std::uint64_t utc_ms);
};
}
