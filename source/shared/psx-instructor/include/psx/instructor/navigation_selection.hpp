#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <cstdint>

namespace psx::instructor {
struct NavigationSelection {
  std::filesystem::path folder;
  std::string name;
  std::uint32_t pid{};
  std::uint64_t process_start_unix_ms{};
  std::filesystem::path metadata_directory;
  bool operator==(const NavigationSelection &) const = default;
};
// Shared local exporter metadata reader. Never guesses from saved preferences.
class NavigationDatabaseSelection {
  std::filesystem::path directory_;
  bool discover_runtime_{};
public:
  explicit NavigationDatabaseSelection(std::filesystem::path directory = {});
  static std::filesystem::path default_directory();
  static NavigationSelection parse(std::string_view, std::uint32_t pid, std::uint64_t start_ms);
  NavigationSelection read(const std::string &host, std::uint16_t main_port) const;
};
}
