#include <psx/instructor/airport_database.hpp>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace psx::instructor {
std::uint64_t DatabaseReader::u(std::size_t p, unsigned n) const {
  std::uint64_t value{};
  for (unsigned k = 0; k < n; ++k) value = (value << 8) | static_cast<unsigned char>(bytes.at(p + k));
  return value;
}
float DatabaseReader::f(std::size_t p) const { return std::bit_cast<float>(std::uint32_t(u(p, 4))); }
double DatabaseReader::d(std::size_t p) const { return std::bit_cast<double>(u(p, 8)); }
int DatabaseReader::s(std::size_t p) const { return std::bit_cast<std::int16_t>(std::uint16_t(u(p, 2))); }
Point DatabaseReader::point(std::size_t p) const {
  Point value{f(p), f(p + 4)};
  if (!std::isfinite(value.lat) || !std::isfinite(value.lon) ||
      std::abs(value.lat) > std::numbers::pi / 2 + 1E-7 || std::abs(value.lon) > std::numbers::pi + 1E-7)
    throw std::runtime_error("Database coordinate invalid");
  return value;
}
std::string DatabaseReader::text(std::size_t p, std::size_t n) const {
  std::string value;
  for (std::size_t k = 0; k < n; ++k) {
    auto c = 127 - u(p + k, 1);
    if (c < 32 || c > 126) throw std::runtime_error("Database text invalid");
    value += char(c);
  }
  while (!value.empty() && value.back() == ' ') value.pop_back();
  return value;
}
std::string DatabaseReader::runway(std::size_t p) const {
  auto n = u(p, 1), c = u(p + 1, 1);
  if (n < 1 || n > 36 || (c != ' ' && c != 'L' && c != 'C' && c != 'R' && c != 'T' && c != 'W'))
    throw std::runtime_error("Database runway identifier invalid");
  return (n < 10 ? "0" : "") + std::to_string(n) + (c == ' ' ? "" : std::string(1, char(c)));
}
DatabaseReader DatabaseReader::file(const std::filesystem::path &path, std::size_t record) {
  auto size = std::filesystem::file_size(path);
  if (!record || size < 37 || size > 64 * 1024 * 1024 || (size - 37) % record)
    throw std::runtime_error("Database record size invalid: " + path.filename().string());
  std::ifstream stream(path, std::ios::binary);
  DatabaseReader result;
  result.bytes.resize(std::size_t(size));
  if (!stream.read(result.bytes.data(), std::streamsize(size))) throw std::runtime_error("Database read failed");
  return result;
}
Runway decode_runway(const DatabaseReader &pg, std::size_t p) {
  Runway r;
  r.id = pg.runway(p + 12); r.threshold = pg.point(p); r.heading = pg.d(p + 18);
  r.elevation = pg.s(p + 26); r.length = std::bit_cast<std::int32_t>(std::uint32_t(pg.u(p + 14, 4)));
  r.width = pg.s(p + 30); r.displaced = pg.s(p + 28); r.crossing = int(pg.u(p + 34, 1));
  if (!std::isfinite(r.heading) || r.heading < 0 || r.heading > 2 * std::numbers::pi + 1E-7)
    throw std::runtime_error("Runway geometry invalid");
  if (r.heading >= 2 * std::numbers::pi) r.heading = 0;
  return r;
}
AirportDatabase::AirportDatabase(const std::filesystem::path &folder)
    : airports_(DatabaseReader::file(folder / "pa.w", 57)), runways_(DatabaseReader::file(folder / "pg.w", 45)) {}
Runway AirportDatabase::lookup(std::wstring_view input) const {
  constexpr auto whitespace = L" \t\r\n";
  const auto first = input.find_first_not_of(whitespace);
  const auto invalid = "Enter airport and runway, e.g. ELLX24";
  if (first == std::wstring_view::npos) throw std::invalid_argument(invalid);
  input = input.substr(first, input.find_last_not_of(whitespace) - first + 1);
  std::string value;
  for (auto c : input) {
    if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    if (!((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))) throw std::invalid_argument(invalid);
    value += static_cast<char>(c);
  }
  if (value.size() < 6 || value.size() > 7 || value[4] < '0' || value[4] > '3' || value[5] < '0' || value[5] > '9')
    throw std::invalid_argument(invalid);
  const int number = (value[4] - '0') * 10 + value[5] - '0';
  const char suffix = value.size() == 7 ? value[6] : ' ';
  if (number < 1 || number > 36 || std::string_view(" LCRTW").find(suffix) == std::string_view::npos)
    throw std::invalid_argument(invalid);
  const auto airport = value.substr(0, 4), runway = value.substr(4);
  bool found{};
  for (std::size_t p = 37; p < airports_.bytes.size(); p += 57)
    if (airports_.text(p + 10, 4) == airport) { found = true; break; }
  if (!found) throw std::runtime_error("airport not found");
  for (std::size_t p = 37; p < runways_.bytes.size(); p += 45)
    if (runways_.text(p + 8, 4) == airport && runways_.runway(p + 12) == runway) return decode_runway(runways_, p);
  throw std::runtime_error("runway not found");
}
}
