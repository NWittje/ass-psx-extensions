#include <ios/airport.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <windows.h>
#include <bcrypt.h>
namespace ios {
namespace {
constexpr double pi = std::numbers::pi;
constexpr double rad_nm = 3437.746770784939; // PSX Calc.RAD2NM
constexpr double nm_ft = 1.0 / 1.6457883E-4; // PSX Calc.FT2NM
double wrap(double x) { return std::remainder(x, 2 * pi); }
bool valid(Point p) {
  return std::isfinite(p.lat) && std::isfinite(p.lon) &&
         std::abs(p.lat) <= pi / 2 + 1E-7 && std::abs(p.lon) <= pi + 1E-7;
}
using Reader = psx::instructor::DatabaseReader;
Reader file(const std::filesystem::path &p, std::size_t record) { return Reader::file(p, record); }

double bearing(Point a, Point b) {
  auto dl = wrap(b.lon - a.lon);
  return std::atan2(std::sin(dl) * std::cos(b.lat),
                   std::cos(a.lat) * std::sin(b.lat) -
                   std::sin(a.lat) * std::cos(b.lat) * std::cos(dl));
}
std::string list(const Airport &a) {
  return "{\"icao\":" + quote(a.id) + ",\"name\":" + quote(a.name) + "}";
}
} // namespace
std::string number(double n) {
  if (!std::isfinite(n)) throw std::invalid_argument("Nonfinite value");
  char b[64];
  auto r = std::to_chars(b, b + sizeof(b), n, std::chars_format::general, 15);
  if (r.ec != std::errc{}) throw std::invalid_argument("Number cannot be encoded");
  return {b, r.ptr};
}
std::string sha256(std::string_view s) {
  BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    throw std::runtime_error("SHA-256 provider unavailable");
  unsigned char bytes[32];
  auto result = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  if (result >= 0) result = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(s.data())), ULONG(s.size()), 0);
  if (result >= 0) result = BCryptFinishHash(hash, bytes, sizeof(bytes), 0);
  if (hash) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);
  if (result < 0) throw std::runtime_error("SHA-256 failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto c : bytes) { out += hex[c >> 4]; out += hex[c & 15]; }
  return out;
}
std::string airport_id(std::string_view s, bool prefix) {
  if ((!prefix && s.empty()) || s.size() > 4)
    throw std::invalid_argument("Airport identifier must contain one to four characters");
  std::string out;
  for (auto c : s) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      throw std::invalid_argument("Invalid ICAO prefix");
    out += c;
  }
  return out;
}
Point destination(Point p, double hdg, double nm) {
  auto a = nm / rad_nm;
  auto lat = std::asin(std::clamp(std::sin(p.lat) * std::cos(a) +
                                 std::cos(p.lat) * std::sin(a) * std::cos(hdg), -1., 1.));
  auto lon = wrap(p.lon + std::atan2(std::sin(hdg) * std::sin(a) * std::cos(p.lat),
                                    std::cos(a) - std::sin(p.lat) * std::sin(lat)));
  return {lat, lon};
}
double distance_nm(Point a, Point b) {
  auto sl = std::sin((b.lat - a.lat) / 2), so = std::sin(wrap(b.lon - a.lon) / 2);
  return 2 * rad_nm * std::asin(std::sqrt(std::clamp(sl * sl + std::cos(a.lat) * std::cos(b.lat) * so * so, 0., 1.)));
}
std::shared_ptr<const Catalogue> Catalogue::load(const std::filesystem::path &folder) {
  auto c = std::make_shared<Catalogue>(); c->folder = std::filesystem::weakly_canonical(folder);
  std::array<std::filesystem::file_time_type, 3> stamps{};
  unsigned stamp{};
  for (auto name : {"pa.w", "pg.w", "pi.w"}) stamps[stamp++] = std::filesystem::last_write_time(folder / name);
  auto pa = file(folder / "pa.w", 57), pg = file(folder / "pg.w", 45), ils = file(folder / "pi.w", 52);
  stamp = 0;
  for (auto name : {"pa.w", "pg.w", "pi.w"})
    if (stamps[stamp++] != std::filesystem::last_write_time(folder / name))
      throw std::runtime_error("Database changed during indexing; retry with a consistent dataset");
  c->hashes = {sha256(pa.bytes), sha256(pg.bytes), sha256(ils.bytes)};
  c->identity = sha256(c->hashes[0] + c->hashes[1] + c->hashes[2]);
  std::map<std::string, std::string_view> airport_records;
  for (std::size_t p = 37; p < pa.bytes.size(); p += 57) {
    Airport a;
    a.id = airport_id(pa.text(p + 10, 4)); a.name = pa.text(p + 25, 30);
    a.reference = pa.point(p); a.elevation = pa.s(p + 14);
    auto raw = std::string_view(pa.bytes).substr(p, 57);
    auto [record, inserted] = airport_records.emplace(a.id, raw);
    if (!inserted) {
      if (record->second != raw) throw std::runtime_error("Conflicting airport records in database");
      continue; // Identical repeated XS94 exists in both installed datasets.
    }
    c->airports.emplace(a.id, std::move(a));
  }
  if ((pg.bytes.size() - 37) % 90) throw std::runtime_error("Unpaired runway database");
  for (std::size_t p = 37; p < pg.bytes.size(); p += 90) {
    auto id = airport_id(pg.text(p + 8, 4));
    if (id != pg.text(p + 53, 4)) throw std::runtime_error("Runway pair airport mismatch");
    auto it = c->airports.find(id);
    if (it == c->airports.end()) throw std::runtime_error("Runway airport missing");
    auto &ends = it->second.runways; auto base = ends.size();
    for (unsigned k = 0; k < 2; ++k) {
      auto q = p + k * 45; RunwayEnd r;
      static_cast<psx::instructor::Runway &>(r) = psx::instructor::decode_runway(pg, q);
      r.opposite = base + (1 - k);
      if (!std::isfinite(r.heading) || r.heading < 0 || r.heading > 2 * pi + 1E-7 ||
          r.length <= 0 || r.width < 0 || r.displaced < 0 || r.displaced > r.length)
        throw std::runtime_error("Runway geometry invalid");
      if (std::any_of(ends.begin(), ends.end(), [&](const auto &e) { return e.id == r.id; }))
        throw std::runtime_error("Duplicate runway end");
      ends.push_back(std::move(r));
    }
  }
  for (std::size_t p = 37; p < ils.bytes.size(); p += 52) {
    auto id = airport_id(ils.text(p + 8, 4)); auto it = c->airports.find(id);
    if (it == c->airports.end()) continue; // radio records can reference uncatalogued airports
    Glidepath g;
    try { g.runway = ils.runway(p + 19); } catch (const std::runtime_error &) { continue; }
    g.id = ils.text(p + 12, 4);
    g.transmitter = {ils.f(p + 29), ils.f(p + 33)}; g.angle = ils.f(p + 37); g.elevation = ils.s(p + 41);
    if (valid(g.transmitter) && std::isfinite(g.angle) && g.angle > 0 && g.angle <= 1.3962634 &&
        std::any_of(it->second.runways.begin(), it->second.runways.end(), [&](const auto &r) { return r.id == g.runway; }))
      it->second.glidepaths.push_back(std::move(g));
  }
  return c;
}
const Airport &Catalogue::airport(std::string_view id) const {
  auto i = airports.find(airport_id(id));
  if (i == airports.end()) throw std::out_of_range("Airport not in selected database");
  return i->second;
}
bool Airport::eligible() const {
  return std::any_of(runways.begin(),runways.end(),[](const auto &r){return r.length*0.3048>=1500;});
}
std::string Catalogue::search(std::string_view query,std::optional<Point> center) const {
  auto prefix = airport_id(query, true); std::string s = "["; unsigned n{};
  auto begin=airports.lower_bound(prefix);std::string nearest;
  if(center&&prefix.empty()) {
    double closest=std::numeric_limits<double>::infinity();
    auto found=airports.end();
    for(auto i=airports.begin();i!=airports.end();++i)if(i->second.eligible()) {
      const auto distance=distance_nm(*center,i->second.reference);
      if(distance<closest){closest=distance;found=i;}
    }
    if(found!=airports.end()) {nearest=found->first;begin=found;unsigned before{};
      while(begin!=airports.begin()&&before<25){--begin;if(begin->second.eligible())++before;}
    }
  }
  for (auto i = begin; i != airports.end() && i->first.starts_with(prefix) && n < 50; ++i) {
    if(!i->second.eligible())continue;
    if (n++) s += ',';auto row=list(i->second);
    if(i->first==nearest){row.pop_back();row+=",\"nearest\":true}";}s+=row;
  }
  return s + "]";
}
std::string Catalogue::detail(std::string_view id) const {
  const auto &a = airport(id); auto s = list(a); s.pop_back();
  s += ",\"dataset\":" + quote(identity) + ",\"elevation_ft\":" + std::to_string(a.elevation) + ",\"runways\":[";
  for (std::size_t n = 0; n < a.runways.size(); ++n) {
    const auto &r = a.runways[n]; auto b = bearing(a.reference, r.threshold), d = distance_nm(a.reference, r.threshold) * 1852;
    if (n) s += ',';
    s += "{\"id\":" + quote(r.id) + ",\"opposite\":" + std::to_string(r.opposite) +
         ",\"east_m\":" + number(d * std::sin(b)) + ",\"north_m\":" + number(d * std::cos(b)) +
         ",\"heading_deg\":" + number(r.heading * 180 / pi) + ",\"elevation_ft\":" + std::to_string(r.elevation) +
         ",\"length_ft\":" + std::to_string(r.length) + ",\"width_ft\":" + std::to_string(r.width) +
         ",\"displaced_ft\":" + std::to_string(r.displaced) + "}";
  }
  return s + "]}";
}
Target target(const Catalogue &c, std::string_view id, std::string_view runway,
              std::string_view preset, double ias, bool manual, double height) {
  const auto &a = c.airport(id);
  auto i = std::find_if(a.runways.begin(), a.runways.end(), [&](const auto &r) { return r.id == runway; });
  if (i == a.runways.end()) throw std::invalid_argument("Select a database runway");
  Target t; t.airport = a.id; t.runway = i->id; t.preset = preset; t.position = i->threshold;
  t.heading = i->heading >= 2 * pi ? 0 : i->heading; t.threshold_elevation = i->elevation; t.source = "Runway threshold";
  if (i->elevation < -2500 || i->elevation > 55000)
    throw std::invalid_argument("Runway elevation is outside native altitude limits (-2,500 to 55,000 ft MSL)");
  if (preset == "takeoff") { t.ground = true; t.ias = 0; t.altitude = i->elevation; return t; }
  if (!std::isfinite(ias) || ias <= 0 || ias > 1000) throw std::invalid_argument("Enter IAS (greater than zero, at most 1,000 kt)");
  t.ias = ias;
  // Instructor-selected IAS schedule, independent of destination TAS conversion.
  t.pitch_deg = 6 - 0.08 * (std::clamp(ias, 150., 200.) - 150);
  t.bank_deg = 0;
  if (preset == "final3" || preset == "final6") {
    t.distance = preset == "final3" ? 3 : 6;
    t.position = destination(i->threshold, i->heading + pi, t.distance);
    double lowest = INFINITY; std::string source;
    for (const auto &g : a.glidepaths) if (g.runway == i->id) {
      auto alt = g.elevation + distance_nm(g.transmitter, t.position) * nm_ft * std::tan(g.angle);
      if (alt < lowest) { lowest = alt; source = "ILS " + g.id + " (" + number(g.angle * 180 / pi) + " degrees), minus 100 ft"; }
    }
    if (std::isfinite(lowest)) { t.height = lowest - i->elevation - 100; t.source = source; }
    else { t.height = t.distance * nm_ft * std::tan(3 * pi / 180) + 50 - 100; t.source = "Nominal 3-degree fallback, 50 ft threshold crossing, minus 100 ft"; }
  } else if (preset == "over5000" || preset == "over10000") {
    t.height = preset == "over5000" ? 5000 : 10000;
  } else throw std::invalid_argument("Unknown position preset");
  if (manual) {
    if (!std::isfinite(height) || height <= 0 || height > 100000)
      throw std::invalid_argument("Manual height must be greater than zero and at most 100,000 ft");
    t.height = height; t.manual = true; t.source = "Manual height override";
  }
  if (t.height <= 0) throw std::invalid_argument("Calculated airborne height is not positive");
  t.altitude = i->elevation + t.height;
  if (!std::isfinite(t.altitude) || t.altitude < -2500 || t.altitude > 55000)
    throw std::invalid_argument("Target altitude is outside native altitude limits (-2,500 to 55,000 ft MSL)");
  return t;
}
std::string Target::json() const {
  return "{\"icao\":" + quote(airport) + ",\"runway\":" + quote(runway) + ",\"preset\":" + quote(preset) +
         ",\"ground\":" + (ground ? "true" : "false") + ",\"manual\":" + (manual ? "true" : "false") +
         ",\"distance_nm\":" + number(distance) + ",\"ias_kt\":" + number(ias) +
         ",\"attitude_mode\":" + quote(ground ? "native_ground" : "fixed") +
         ",\"pitch_deg\":" + (ground ? "null" : number(pitch_deg)) +
         ",\"bank_deg\":" + (ground ? "null" : number(bank_deg)) +
         ",\"height_ft\":" + number(height) + ",\"altitude_msl_ft\":" + number(altitude) +
         ",\"heading_true_deg\":" + number(heading * 180 / pi) + ",\"source\":" + quote(source) + "}";
}
} // namespace ios
