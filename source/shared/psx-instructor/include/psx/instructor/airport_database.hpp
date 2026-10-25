#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace psx::instructor {
struct Point { double lat{}, lon{}; };
struct DatabaseReader {
  std::string bytes;
  std::uint64_t u(std::size_t, unsigned) const;
  float f(std::size_t) const;
  double d(std::size_t) const;
  int s(std::size_t) const;
  Point point(std::size_t) const;
  std::string text(std::size_t, std::size_t) const;
  std::string runway(std::size_t) const;
  static DatabaseReader file(const std::filesystem::path &, std::size_t record);
};
struct Runway {
  std::string id;
  Point threshold;
  double heading{};
  int elevation{}, length{}, width{}, displaced{}, crossing{};
};
Runway decode_runway(const DatabaseReader &, std::size_t);
// Minimal pa.w/pg.w lookup, also usable without the IOS catalogue or ILS database.
class AirportDatabase {
  DatabaseReader airports_, runways_;
public:
  explicit AirportDatabase(const std::filesystem::path &);
  Runway lookup(std::wstring_view airport_runway) const;
};
}
