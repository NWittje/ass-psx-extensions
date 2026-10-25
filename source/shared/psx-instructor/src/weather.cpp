#include <psx/instructor/weather.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>

namespace psx::instructor {
namespace {
constexpr double hpa_per_raw = 0.33863887;
struct Field { const char *name; unsigned token; int low, high; };
constexpr Field fields[]{
  {"Lower base", 5, 0, 45000}, {"Lower top", 4, 0, 45000}, {"Lower coverage", 3, 0, 8},
  {"Upper base", 2, 100, 45100}, {"Upper top", 1, 100, 45100}, {"Upper coverage", 0, 0, 8},
  {"CB base", 11, 0, 45000}, {"CB top", 10, 0, 45000}, {"CB coverage", 9, 0, 8},
  {"QNH", 23, 2700, 3200}, {"Surface temperature", 22, -80, 50}, {"Visibility", 20, 0, 9999},
  {"Precipitation", 21, 0, 3}, {"Wind direction", 18, 0, 359}, {"Wind variation", 18, 0, 359},
  {"Wind speed", 18, 0, 75}, {"Gust", 19, 0, 75}, {"Inversion enabled", 15, 0, 1},
  {"Inversion height", 16, 500, 3500}, {"Inversion temperature", 17, -50, 50}
};
const Field &field(WeatherField f) {
  auto n = static_cast<unsigned>(f);
  if (n >= std::size(fields)) throw std::invalid_argument("Unknown weather field");
  return fields[n];
}
std::vector<std::string_view> split(std::string_view s) {
  if (s.find_first_of("\r\n\0", 0, 3) != s.npos) throw std::invalid_argument("Invalid weather text");
  std::vector<std::string_view> out;
  for (;;) { auto p = s.find(';'); out.push_back(s.substr(0, p));
    if (p == s.npos) break; s.remove_prefix(p + 1); }
  return out;
}
int number(std::string_view s) {
  auto n = integer(s);
  if (!n) throw std::invalid_argument("Invalid weather integer");
  return *n;
}
double real(std::string_view s) {
  double v{}; auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v))
    throw std::invalid_argument("Invalid weather coordinate");
  return v;
}
std::string decimal(double v, int precision = std::numeric_limits<double>::max_digits10) {
  char b[64]; auto r = std::to_chars(b, b + sizeof(b), v, std::chars_format::general, precision);
  if (r.ec != std::errc{}) throw std::invalid_argument("Weather number cannot be encoded");
  return {b, r.ptr};
}
bool ident(std::string_view s) {
  return s.size() == 4 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}
void coordinates(double lat, double lon) {
  if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > std::numbers::pi / 2 + 1e-7 ||
      std::abs(lon) > std::numbers::pi + 1e-7) throw std::invalid_argument("Weather coordinates out of range");
}
std::array<int, 3> wind(std::string_view s) {
  if (s.size() < 7 || s.size() > 8 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
    throw std::invalid_argument("Invalid packed wind");
  return {number(s.substr(0, 3)), number(s.substr(3, 3)), number(s.substr(6))};
}
std::string padded(int n) { auto s = std::to_string(n); return std::string(3 - s.size(), '0') + s; }
void validate(const WeatherSettings &s) {
  for (unsigned n = 0; n < std::size(fields); ++n) {
    auto f = static_cast<WeatherField>(n); auto v = f == WeatherField::qnh ? s.pressure_raw() : s.value(f);
    if (v < fields[n].low || v > fields[n].high) throw std::invalid_argument(std::string(fields[n].name) + " out of range");
  }
  const auto &t = s.tokens();
  constexpr unsigned extra[]{6, 7, 8, 12, 13, 14};
  constexpr int lo[]{0, 0, 0, 0, 0, 100}, hi[]{3, 45000, 45000, 3, 100, 1300};
  for (unsigned k = 0; k < std::size(extra); ++k) {
    int v = number(t[extra[k]]);
    if (v < lo[k] || v > hi[k]) throw std::invalid_argument("Preserved weather field out of range");
  }
  if (number(t[2]) > number(t[1]) || number(t[5]) > number(t[4]) ||
      number(t[11]) > number(t[10]) || number(t[8]) > number(t[7]))
    throw std::invalid_argument("Weather base exceeds top");
  if (number(t[4]) >= number(t[2])) throw std::invalid_argument("Lower clouds must be below upper cloud base");
  if (s.value(WeatherField::gust) < s.value(WeatherField::wind_speed))
    throw std::invalid_argument("Gust must be at least mean wind speed");
  auto length = s.encode().size();
  if (length < 60 || length > 107) throw std::invalid_argument("Weather record length out of range");
}
WeatherSettings edited(const WeatherSettings &base, std::span<const WeatherEdit> edits) {
  auto tokens = base.tokens(); auto w = wind(tokens[18]); bool changed_wind = false;
  for (const auto &e : edits) {
    const auto &f = field(e.field);
    if (!std::isfinite(e.value)) throw std::invalid_argument(std::string(f.name) + " must be finite");
    double v = e.field == WeatherField::qnh ? std::round(e.value / hpa_per_raw) : e.value;
    if (v < f.low || v > f.high || std::trunc(v) != v) throw std::invalid_argument(std::string(f.name) + " out of range or not an integer");
    int n = static_cast<int>(v);
    if (e.field == WeatherField::wind_variation) { w[0] = n; changed_wind = true; }
    else if (e.field == WeatherField::wind_direction) { w[1] = n; changed_wind = true; }
    else if (e.field == WeatherField::wind_speed) { w[2] = n; changed_wind = true; }
    else tokens[f.token] = std::to_string(n);
  }
  if (changed_wind) tokens[18] = padded(w[0]) + padded(w[1]) + std::to_string(w[2]);
  std::string out;
  for (const auto &t : tokens) { if (!out.empty()) out += ';'; out += t; }
  auto result = WeatherSettings::parse(out); validate(result); return result;
}
std::string station_id(std::string_view encoded) {
  std::string out;
  for (unsigned char c : encoded) out += static_cast<char>(127 - c);
  if (!ident(out)) throw std::invalid_argument("Invalid weather station identifier");
  return out;
}
void station_valid(const WeatherStation &s) {
  coordinates(s.latitude, s.longitude);
  if (!ident(s.identifier) || s.elevation < -32768 || s.elevation > 32767)
    throw std::invalid_argument("Invalid weather station");
}
} // namespace

WeatherSettings WeatherSettings::parse(std::string_view text) {
  if (text.size() < 60 || text.size() > 107) throw std::invalid_argument("Weather record length out of range");
  auto parts = split(text);
  if (parts.size() != 24) throw std::invalid_argument("Expected 24 weather fields");
  WeatherSettings s;
  for (unsigned k = 0; k < 24; ++k) { if (k == 18) wind(parts[k]); else number(parts[k]); s.tokens_[k] = parts[k]; }
  return s;
}
double WeatherSettings::value(WeatherField f) const {
  const auto &spec = field(f);
  if (f == WeatherField::qnh) return pressure_raw() * hpa_per_raw;
  if (spec.token == 18) { auto w = wind(tokens_[18]);
    return w[f == WeatherField::wind_variation ? 0 : f == WeatherField::wind_direction ? 1 : 2]; }
  return number(tokens_[spec.token]);
}
int WeatherSettings::pressure_raw() const { return number(tokens_[23]); }
int WeatherSettings::qnh_display_hpa() const { return static_cast<int>(std::round(static_cast<float>(pressure_raw()) * 0.33863887f)); }
std::string WeatherSettings::encode() const {
  std::string s; for (const auto &t : tokens_) { if (!s.empty()) s += ';'; s += t; } return s;
}
WeatherZone WeatherZone::parse(std::string_view text) {
  if (text.size() < 10 || text.size() > 70) throw std::invalid_argument("Zone record length out of range");
  auto t = split(text); if (t.size() != 6) throw std::invalid_argument("Expected six zone fields");
  double lat = real(t[0]), lon = real(t[1]); coordinates(lat, lon);
  WeatherZone z{static_cast<float>(lat), static_cast<float>(lon), number(t[2]), number(t[3]), number(t[4]), std::string(t[5])};
  if (z.radius < 10 || z.radius > 1000 || z.elevation < -2000 || z.elevation > 20000 || z.month < 0 || z.month > 12 ||
      z.identifier.size() != 7 || number(std::string_view(z.identifier).substr(4, 2)) < 0 ||
      number(std::string_view(z.identifier).substr(4, 2)) > 36 ||
      (z.identifier.back() != 'p' && z.identifier.back() != 'P')) throw std::invalid_argument("Invalid zone options");
  // Native special centre identifiers (e.g. ----) are retained unchanged.
  if (!std::all_of(z.identifier.begin(), z.identifier.end(), [](unsigned char c) { return c >= 32 && c <= 126; }))
    throw std::invalid_argument("Invalid zone identifier");
  return z;
}
std::string WeatherZone::encode() const {
  auto s = decimal(latitude, std::numeric_limits<float>::max_digits10) + ';' +
    decimal(longitude, std::numeric_limits<float>::max_digits10) + ';' + std::to_string(radius) + ';' +
    std::to_string(elevation) + ';' + std::to_string(month) + ';' + identifier;
  parse(s); return s;
}
WeatherStations WeatherStations::load(const std::filesystem::path &root) {
  WeatherStations out;
  if (std::filesystem::exists(root / "Weather/stations.txt")) {
    auto path = root / "Weather/stations.txt";
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::invalid_argument("Weather stations file too large");
    std::ifstream file(path); if (!file) throw std::runtime_error("Weather stations unavailable");
    std::string line; std::getline(file, line); // native skips the header
    while (out.size() < 20000 && std::getline(file, line)) {
      if (line.size() <= 82 || line[62] != 'X' || line[0] == '!' ||
          (line[81] == 'U' && line[82] == 'S' && (line[20] == 'X' || line[20] == 'Q'))) continue;
      auto n = [&](std::size_t p, std::size_t count) {
        auto s = std::string_view(line).substr(p, count);
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        return number(s);
      };
      // Preserve PSX's float arithmetic and metre-to-foot rounding.
      float lat = (n(39, 2) + n(42, 2) / 60.f) * 0.017453292f;
      float lon = (n(47, 4) + n(51, 2) / 60.f) * 0.017453292f;
      if (line[44] == 'S') lat = -lat; if (line[53] == 'W') lon = -lon;
      int elev = static_cast<int>(std::floor(n(55, 4) * 3.28084f + 0.5f));
      if (elev > 32767 || elev < -32768) throw std::invalid_argument("Weather station elevation out of range");
      WeatherStation s{line.substr(20, 4), lat, lon, elev}; station_valid(s); out.stations_.push_back(s);
    }
  } else {
    auto file = DatabaseReader::file(root / "Navigation/ms.w", 14);
    for (std::size_t p = 37; p < file.bytes.size() && out.size() < 20000; p += 14) {
      WeatherStation s{station_id(std::string_view(file.bytes).substr(p, 4)), file.f(p + 4), file.f(p + 8), file.s(p + 12)};
      station_valid(s); out.stations_.push_back(s);
    }
    // Corr.makeMet adds LTFM to older station tables except the 1903 table.
    if (std::string_view(file.bytes).substr(0, 37) != "Copyright 2019 Aerowinx - Cycle 1903" && out.size() < 19999)
      out.stations_.push_back({"LTFM", 0.7203895f, 0.5018161f, 325});
  }
  if (out.stations_.empty()) throw std::runtime_error("Weather station catalogue empty");
  return out;
}
const WeatherStation &WeatherStations::nearest(Point p) const {
  coordinates(p.lat, p.lon);
  if (stations_.empty()) throw std::runtime_error("Weather station catalogue empty");
  // Native button casts aircraft position to float before its distance scan.
  double lat = static_cast<float>(p.lat), lon = static_cast<float>(p.lon);
  const WeatherStation *best = &stations_.front(); double nearest = 99999;
  for (const auto &s : stations_) {
    double a = std::sin((lat - s.latitude) / 2), b = std::sin((lon - s.longitude) / 2);
    double h = a * a + std::cos(lat) * std::cos(s.latitude) * b * b;
    double d = 6875.493541569878 * std::asin(std::sqrt(std::clamp(h, 0., 1.)));
    if (d < nearest) { nearest = d; best = &s; }
  }
  return *best;
}

bool Weather::request_read() {
  if (busy()) throw std::logic_error("Weather sender busy");
  return reader_.send("bang\n");
}
void Weather::finish(WeatherStatus status, std::string detail) {
  result_.status = status; result_.detail = std::move(detail); phase_ = Phase::idle;
}
void Weather::begin(Action action, std::string_view text) {
  if (busy()) throw std::logic_error("Weather sender busy");
  result_ = {}; result_.status = WeatherStatus::pending; result_.detail = "Sending settings";
  action_ = action; phase_ = Phase::sending;
  if (!writer_.send(text)) finish(WeatherStatus::failed, "Weather send failed; no retry");
}
WeatherSettings WeatherSettings::with(std::span<const WeatherEdit> edits) const { return edited(*this, edits); }
void Weather::apply(WeatherTarget target, std::span<const WeatherEdit> edits, bool manual) {
  if (busy()) throw std::logic_error("Weather sender busy");
  const auto &base = settings(target);
  if (!base) throw std::invalid_argument("Weather baseline unavailable; read settings first");
  if (edits.empty()) throw std::invalid_argument("No weather edits");
  auto e = edited(*base, edits);
  edited_.clear(); for (const auto &change : edits) {
    if (std::find(edited_.begin(), edited_.end(), change.field) == edited_.end()) edited_.push_back(change.field);
  }
  expected_ = e; target_ = target;
  std::string packet = manual ? "Qi139=0\n" : "";
  if (target == WeatherTarget::all) {
    // One complete edited image, including preserved native fields. Zone centres,
    // radii, elevations and METAR identities are separate records and stay intact.
    for (unsigned i=0;i<settings_.size();++i)
      packet += "Qs" + std::to_string(328+i) + '=' + e.encode() + '\n';
  } else packet += std::string(target == WeatherTarget::planet ? "Qs328=" : "Qs329=") + e.encode() + '\n';
  begin(Action::apply, packet);
}
void Weather::set_nearest_airport(const WeatherStations &stations, Tick t) {
  if (busy()) throw std::logic_error("Weather sender busy");
  if (!zone_ || !metar_ || !stations.size()) throw std::invalid_argument("Zone 1, METAR baseline or station catalogue unavailable");
  result_ = {}; result_.status = WeatherStatus::pending; result_.detail = "Reading current aircraft position";
  action_ = Action::nearest; stations_ = &stations; phase_ = Phase::locating; deadline_ = t + 5000;
  if (!reader_.send("bang\n")) finish(WeatherStatus::unavailable, "Aircraft position request failed; no retry");
}
void Weather::place_nearest() {
  const auto &s = stations_->nearest(*position_);
  auto z = *zone_; z.latitude = s.latitude; z.longitude = s.longitude; z.elevation = s.elevation;
  z.radius = 320; z.identifier.replace(0, 4, s.identifier);
  std::size_t skip = metar_->front() == 'i' ? 5 : 4;
  if (metar_->size() < skip) throw std::invalid_argument("Zone 1 METAR lacks station identifier");
  auto text = s.identifier + metar_->substr(skip);
  if (text.size() > 200) throw std::invalid_argument("Zone 1 METAR too long");
  auto packet = "Qs336=" + z.encode() + "\nQs343=" + text + '\n';
  expected_zone_ = z;
  phase_ = Phase::idle;
  begin(Action::nearest, packet);
}
void Weather::select_manual() { begin(Action::manual, "Qi139=0\n"); }
void Weather::cancel() { if(busy()) finish(WeatherStatus::unavailable,"Readback cancelled; no replay"); }
void Weather::disconnected() {
  settings_ = {}; zone_.reset(); automatic_.reset(); focused_.reset(); position_.reset(); metar_.reset();
  if (busy()) finish(WeatherStatus::unavailable, "Weather readback unavailable: disconnected; no replay");
}
void Weather::record(std::string_view line) {
  if (line == "load1" || line == "exit") { disconnected(); return; }
  try {
    const auto record = line.size()>=6 && line.starts_with("Qs") && line[5]=='=' ? integer(line.substr(2,3)) : std::nullopt;
    if (record && *record>=328 && *record<=335) {
      const auto index = static_cast<unsigned>(*record-328);
      auto &s = settings_[index];
      s.reset(); s = WeatherSettings::parse(line.substr(6));
      if (phase_ == Phase::reading && action_ == Action::apply) {
        read_settings_ |= 1u<<index;
        if (target_ == WeatherTarget::all || index == (target_ == WeatherTarget::planet ? 0u : 1u)) compare();
      }
    } else if (line.starts_with("Qs336=")) {
      zone_.reset(); zone_ = WeatherZone::parse(line.substr(6));
      if (phase_ == Phase::reading && action_ == Action::nearest) { read_zone_ = true; compare(); }
    } else if (line.starts_with("Qs343=")) {
      metar_.reset(); auto s = line.substr(6);
      if (s.empty() || s.size() > 200 || s.find_first_of("\r\n\0", 0, 3) != s.npos)
        throw std::invalid_argument("Invalid Zone 1 METAR");
      metar_ = s;
      if (phase_ == Phase::reading && action_ == Action::nearest) { read_metar_ = true; compare(); }
    } else if (line.starts_with("Qi139=")) {
      automatic_.reset(); int n = number(line.substr(6));
      if (n != 0 && n != 1) throw std::invalid_argument("Invalid automatic weather mode");
      automatic_ = n != 0;
      if (phase_ == Phase::reading && action_ == Action::apply && target_ == WeatherTarget::all) { read_manual_=true; compare(); }
      if (phase_ == Phase::reading && action_ == Action::manual) compare();
    } else if (line.starts_with("Qi240=")) {
      focused_.reset(); int n = number(line.substr(6));
      if (n < 0 || n > 7) throw std::invalid_argument("Invalid focused weather zone"); focused_ = n;
    } else if (line.starts_with("Qs122=")) {
      position_.reset(); auto t = split(line.substr(6));
      if (t.size() < 10) throw std::invalid_argument("Invalid aircraft position");
      Point p{real(t[8]), real(t[9])}; coordinates(p.lat, p.lon); position_ = p;
      if (phase_ == Phase::locating) place_nearest();
    }
  } catch (const std::invalid_argument &e) {
    bool required = (action_ == Action::apply && (target_ == WeatherTarget::all ? (line.starts_with("Qs33") || line.starts_with("Qs328=") || line.starts_with("Qs329=") || line.starts_with("Qi139=")) : line.starts_with(target_ == WeatherTarget::planet ? "Qs328=" : "Qs329="))) ||
      (action_ == Action::nearest && (line.starts_with("Qs336=") || line.starts_with("Qs343="))) ||
      (action_ == Action::manual && line.starts_with("Qi139="));
    if (phase_ == Phase::reading && required) finish(WeatherStatus::unavailable, std::string("Malformed weather readback: ") + e.what());
    else if (phase_ == Phase::locating) finish(WeatherStatus::unavailable, std::string("Nearest airport unavailable: ") + e.what());
  }
}
void Weather::compare() {
  auto diff = [&](const char *name, std::string requested, std::string returned) {
    if (requested != returned) result_.differences.push_back({name, std::move(requested), std::move(returned)});
  };
  if (action_ == Action::apply) {
    if (target_ == WeatherTarget::all) {
      if (read_settings_ != 255u || !read_manual_) return;
      diff("Automatic weather", "0", automatic_.value_or(true) ? "1" : "0");
      for (unsigned i=0;i<settings_.size();++i) {
        const auto label = i ? "Zone " + std::to_string(i) : std::string("Planet");
        diff(label.c_str(), expected_->encode(), settings_[i]->encode());
      }
      result_.settings = settings_[0];
    } else {
    const auto &s = *settings(target_); result_.settings = s;
    for (auto f : edited_) {
      if (f == WeatherField::qnh) diff("QNH (inHg x100)", std::to_string(expected_->pressure_raw()), std::to_string(s.pressure_raw()));
      else diff(field(f).name, decimal(expected_->value(f)), decimal(s.value(f)));
    }
    }
  } else if (action_ == Action::nearest) {
    if (!read_zone_ || !read_metar_) return;
    const auto &z = *zone_; const auto &e = *expected_zone_; result_.zone = z;
    result_.metar = metar_;
    diff("Latitude", decimal(e.latitude), decimal(z.latitude)); diff("Longitude", decimal(e.longitude), decimal(z.longitude));
    diff("Elevation", std::to_string(e.elevation), std::to_string(z.elevation));
    diff("Radius", std::to_string(e.radius), std::to_string(z.radius));
    diff("Month", std::to_string(e.month), std::to_string(z.month)); diff("Airport/options", e.identifier, z.identifier);
    std::string_view metar = *metar_; if (metar.starts_with('i')) metar.remove_prefix(1);
    diff("METAR station", e.identifier.substr(0, 4), std::string(metar.substr(0, 4)));
  } else diff("Automatic weather", "0", *automatic_ ? "1" : "0");
  finish(result_.differences.empty() ? WeatherStatus::match : WeatherStatus::different,
    result_.differences.empty() ? "Settings read back match; actual atmosphere not verified" : "Returned settings differ; no retry");
}
void Weather::pump(Tick t) {
  if (!busy()) return;
  if (writer_.state() != network::State::connected || reader_.state() != network::State::connected) { disconnected(); return; }
  if (phase_ == Phase::locating && t >= deadline_) {
    finish(WeatherStatus::unavailable, "Aircraft position unavailable within five seconds; no placement");
  } else if (phase_ == Phase::sending && !writer_.sending()) {
    phase_ = Phase::waiting; deadline_ = t + 1000; result_.detail = "Waiting one second after transmission";
  } else if (phase_ == Phase::waiting && t >= deadline_) {
    read_zone_ = false; read_metar_ = false; read_settings_=0; read_manual_=false;
    phase_ = Phase::reading; deadline_ = t + 5000; result_.detail = "Reading configured settings";
    if (!reader_.send("bang\n")) finish(WeatherStatus::unavailable, "Weather readback request failed; no retry");
  } else if (phase_ == Phase::reading && t >= deadline_) finish(WeatherStatus::unavailable, "Weather readback unavailable within five seconds; no retry");
}
} // namespace psx::instructor
