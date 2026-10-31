#include <ios/hwio_motors.hpp>
#include <algorithm>
#include <fstream>
#include <ios/runtime.hpp>
#include <ios/non_normal.hpp>
#include <ios/weight_service.hpp>
#include <ios/visibility.hpp>
#include <ios/status_readout.hpp>
#include <ios/fmc_quicksave.hpp>
#include <sstream>
#include <cmath>
#include <random>
namespace ios {
std::wstring wide(std::string_view s) {
  if (s.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0);
  if (!n)
    throw std::invalid_argument("Invalid UTF-8");
  std::wstring out(std::size_t(n), 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      out.data(), n);
  return out;
}
std::string narrow(std::wstring_view s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                              int(s.size()), nullptr, 0, nullptr, nullptr);
  if (!n)
    throw std::invalid_argument("Invalid Unicode");
  std::string out(std::size_t(n), 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()),
                      out.data(), n, nullptr, nullptr);
  return out;
}
namespace {
std::string read(const std::filesystem::path &p) {
  if (!std::filesystem::exists(p))
    return {};
  if (std::filesystem::file_size(p) > 1024 * 1024)
    throw std::runtime_error("Configuration file too large");
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error("Cannot read configuration");
  return {std::istreambuf_iterator<char>(f), {}};
}
void validate(const Config &c) {
  if (!c.main_port || !c.web_port)
    throw std::invalid_argument("Port must be 1..65535");
  psx::network::Endpoint::parse(c.psx, c.main_port);
  if (c.adapter.size() > 128 ||
      c.adapter.find_first_of("\r\n\t=") != std::string::npos)
    throw std::invalid_argument("Invalid adapter GUID");
  if (c.database.size() > 256 || c.database.find_first_of("\r\n\t=") != std::string::npos)
    throw std::invalid_argument("Invalid database folder");
}
} // namespace
psx::network::Adapter main_adapter(const Config &c,
                             const psx::network::Inventory &inv) {
  auto remote = psx::network::Endpoint::parse(c.psx, c.main_port);
  auto h = remote.host();
  if (h == "127.0.0.1" || h == "::1") {
    psx::network::Adapter a;
    a.up = true;
    a.addresses.push_back(psx::network::Endpoint::parse(h, 0));
    return a;
  }
  if (c.adapter.empty())
    return psx::network::route_ipv4(inv, remote);
  for (const auto &a : inv.adapters)
    if (narrow(a.guid) == c.adapter)
      return a;
  return {};
}
Config read_config(const std::filesystem::path &p) {
  Config c;
  auto text = read(p);
  if (text.empty()) {
    if (std::filesystem::exists(p))
      throw std::runtime_error("Empty settings.cfg");
    return c;
  }
  std::istringstream f(text);
  std::string line;
  Object fields;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    auto i = line.find('=');
    if (i == std::string::npos ||
        !fields.emplace(line.substr(0, i), line.substr(i + 1)).second)
      throw std::runtime_error("Invalid/duplicate settings key");
  }
  auto schema = field<std::string>(fields, "schema");
  if (schema == "1") keys(fields, {"schema", "psx", "main_port", "web_port", "adapter_guid"});
  else if (schema == "2") {
    keys(fields, {"schema", "psx", "main_port", "web_port", "adapter_guid", "confirmation", "database_folder"});
    auto v = field<std::string>(fields, "confirmation");
    if (v != "0" && v != "1") throw std::invalid_argument("Invalid confirmation setting");
    c.confirmation = v == "1"; c.database = field<std::string>(fields, "database_folder");
  } else
    throw std::runtime_error("Unsupported settings schema");
  c.psx = field<std::string>(fields, "psx");
  c.adapter = field<std::string>(fields, "adapter_guid");
  auto port = [&](const char *k) {
    auto s = field<std::string>(fields, k);
    auto o = json("{\"n\":" + s + "}");
    auto n = field<std::uint64_t>(o, "n");
    if (n < 1 || n > 65535)
      throw std::invalid_argument("Invalid port");
    return std::uint16_t(n);
  };
  c.main_port = port("main_port");
  c.web_port = port("web_port");
  validate(c);
  return c;
}
std::map<std::string, Profile> read_profiles(const std::filesystem::path &p) {
  std::map<std::string, Profile> result;
  auto text = read(p);
  if (text.empty()) {
    if (std::filesystem::exists(p))
      throw std::runtime_error("Empty clients.cfg");
    return result;
  }
  std::istringstream f(text);
  std::string line;
  if (!std::getline(f, line))
    throw std::runtime_error("Missing clients schema");
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  bool legacy = line == "schema=1";
  bool units = line == "schema=4";
  bool themed = line == "schema=3" || units;
  if (!legacy && line != "schema=2" && !themed)
    throw std::runtime_error("Invalid clients schema");
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    std::istringstream row(line);
    std::string ip, tab, collapsed, extra;
    if (!(row >> ip >> tab >> collapsed) || !valid_tab(tab) ||
        (collapsed != "0" && collapsed != "1"))
      throw std::runtime_error("Invalid clients profile");
    Profile saved{tab, collapsed == "1"};
    if (!legacy) {
      std::string speed, favorites, recent;
      if (!(row >> speed >> favorites >> recent)) throw std::runtime_error("Incomplete clients profile");
      saved.ias = numeric(json("{\"n\":" + speed + "}"), "n");
      if (saved.ias < 0 || saved.ias > 1000) throw std::runtime_error("Invalid saved IAS");
      auto parse = [](const std::string &s, unsigned limit) {
        std::vector<std::string> out; if (s == "-") return out;
        std::istringstream list(s); std::string id;
        while (std::getline(list, id, ',')) {
          id = airport_id(id);
          if (std::find(out.begin(), out.end(), id) != out.end() || out.size() == limit)
            throw std::runtime_error("Invalid airport preferences");
          out.push_back(std::move(id));
        }
        return out;
      };
      saved.favorites = parse(favorites, 32); saved.recent = parse(recent, 20);
    }
    if (themed && (!(row >> saved.theme) || !valid_theme(saved.theme)))
      throw std::runtime_error("Invalid saved theme");
    if (units && (!(row >> saved.weather_units) || !valid_weather_units(saved.weather_units)))
      throw std::runtime_error("Invalid saved weather units");
    if ((row >> extra) || !result.emplace(normalize_ip(ip), std::move(saved)).second)
      throw std::runtime_error("Invalid/duplicate clients profile");
    if (result.size() > 1024)
      throw std::runtime_error("Too many client profiles");
  }
  return result;
}
std::string config_text(const Config &c) {
  return "schema=2\npsx=" + c.psx +
         "\nmain_port=" + std::to_string(c.main_port) +
         "\nweb_port=" + std::to_string(c.web_port) +
         "\nadapter_guid=" + c.adapter + "\nconfirmation=" + (c.confirmation ? "1" : "0") +
         "\ndatabase_folder=" + c.database + "\n";
}
std::string profiles_text(const std::map<std::string, Profile> &p) {
  std::string s = "schema=4\n";
  auto join = [](const std::vector<std::string> &v) {
    std::string text; for (const auto &id : v) { if (!text.empty()) text += ','; text += id; }
    return text.empty() ? "-" : text;
  };
  for (const auto &[ip, v] : p)
    s += ip + '\t' + v.tab + '\t' + (v.collapsed ? "1" : "0") + '\t' + number(v.ias) + '\t' +
         join(v.favorites) + '\t' + join(v.recent) + '\t' + v.theme + '\t' + v.weather_units + '\n';
  return s;
}
void save_text(const std::filesystem::path &p, std::string_view s) {
  if (std::filesystem::exists(p) && read(p) == s)
    return;
  auto tmp = p;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(s.data(), std::streamsize(s.size()));
    f.close();
    if (!f)
      throw std::runtime_error("Cannot write configuration temporary file");
  }
  auto bak = p;
  bak += L".bak";
  if (std::filesystem::exists(p)) {
    if (!ReplaceFileW(p.c_str(), tmp.c_str(), bak.c_str(), 0, nullptr, nullptr))
      throw std::runtime_error(
          "Cannot replace configuration (original retained)");
  } else if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot create configuration");
}
Binding binding(const Config &c, const psx::network::Inventory &inv) {
  Binding b;
  b.port = c.web_port;
  b.addresses = {"127.0.0.1"};
  // Browser listeners are independent of the outgoing PSX adapter. Inventory
  // contains preferred unicast addresses; down adapters never claim a URL.
  for (const auto &a : inv.adapters)
    if (a.up)
      for (const auto &address : a.addresses)
        if (address.address.ss_family == AF_INET)
          b.addresses.push_back(address.host());
  std::sort(b.addresses.begin(), b.addresses.end());
  b.addresses.erase(std::unique(b.addresses.begin(), b.addresses.end()),
                    b.addresses.end());
  if (b.addresses.size() == 1)
    b.notice = "No usable LAN IPv4 address; loopback remains available";
  return b;
}
Runtime::Runtime(std::filesystem::path p, std::filesystem::path navigation_metadata, std::filesystem::path time_data,std::wstring coordinator_endpoint,std::wstring hwio_endpoint,TerrainPreflight::Check terrain_check,std::wstring monitor_endpoint,std::wstring monitor_image)
  : directory_(std::move(p)), time_data_(time_data.empty()?directory_/"environment-time.bin":std::move(time_data)), coordinator_endpoint_(std::move(coordinator_endpoint)),hwio_endpoint_(std::move(hwio_endpoint)),terrain_(std::move(terrain_check)),xplane_health_(std::move(monitor_endpoint),std::move(monitor_image)), database_selection_(navigation_metadata), performance_selection_(navigation_metadata.empty()?std::filesystem::path{}:navigation_metadata.parent_path()/"performance"),instructor_metadata_(navigation_metadata.empty()?std::filesystem::path{}:navigation_metadata.parent_path()/"instructor") {
  std::filesystem::create_directories(directory_);
  config_ = read_config(directory_ / "settings.cfg");
  profiles_ = read_profiles(directory_ / "clients.cfg");
  if (!std::filesystem::exists(directory_ / "settings.cfg") ||
      !std::filesystem::exists(directory_ / "clients.cfg"))
    persist_revision_ = 1;
  persistence_ = std::thread([this] { persist(); });
  try {
    database_ = std::thread([this] { database(); });
    network_ = std::thread([this] { network(); });
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      ending_ = true;
    }
    cv_.notify_all();
    if (database_.joinable()) database_.join();
    persistence_.join();
    throw;
  }
}
Runtime::~Runtime() {
  {
    std::lock_guard lock(mutex_);
    ending_ = true;
  }
  cv_.notify_all();
  if (network_.joinable())
    network_.join();
  if (database_.joinable()) database_.join();
  if (persistence_.joinable())
    persistence_.join();
}
Config Runtime::config() const {
  std::lock_guard lock(mutex_);
  return config_;
}
void Runtime::configure(const Config &c) {
  validate(c);
  {
    std::lock_guard lock(mutex_);
    if (c == config_)
      return;
    bool transport = c.psx != config_.psx || c.adapter != config_.adapter || c.main_port != config_.main_port;
    if (c.psx != config_.psx || c.main_port != config_.main_port) {
      catalogue_.reset(); active_database_ = {}; previews_.clear(); ++database_revision_;
      database_error_ = "Loading active PSX database";
    }
    config_ = c;
    if (transport) ++config_revision_;
    ++persist_revision_;
    if (transport) {
      engine_.lost(0, "Endpoint configuration changed");
      engine_.lost(1, "Endpoint configuration changed");
    }
  }
  cv_.notify_all();
}
void Runtime::control(bool v) {
  if (!v)
    throw std::runtime_error("IOS connects automatically; stopping connections is unavailable");
  {
    std::lock_guard lock(mutex_);
    if(engine_.enabled!=v) ++config_revision_;
    engine_.control(v);
  }
  cv_.notify_all();
}
Binding Runtime::web_binding() const {
  auto i = inventory();
  return binding(config(), i ? *i : psx::network::Inventory{});
}
void Runtime::web_status(std::string listeners, std::string error) {
  std::lock_guard lock(mutex_);
  listeners_ = std::move(listeners);
  web_error_ = std::move(error);
}
WebStatus Runtime::web_status() const {
  std::lock_guard lock(mutex_);
  return {listeners_, web_error_};
}
std::string Runtime::status() const {
  std::lock_guard lock(mutex_);
  auto s = engine_.status(now());
  s.pop_back();
  s += ",\"situations\":"+situations_.status();
  const bool performance_available=engine_.ready()&&flap_speeds_.has_value();
  std::string performance="{\"available\":"+std::string(performance_available?"true":"false")+",\"reason\":"+quote(engine_.ready()?flap_speed_notice_:"PSX is disconnected")+
    ",\"ref25_kt\":"+(performance_available?std::to_string(flap_speeds_->ref25_kt):"null")+",\"ref30_kt\":"+(performance_available?std::to_string(flap_speeds_->ref30_kt):"null")+
    ",\"gross_weight_kg\":"+(performance_available?number(flap_speeds_->gross_weight_kg):"null")+"}";
  return s + ",\"fmc\":"+fmc_json_+",\"flap_speeds\":"+performance+",\"aircraft_position\":"+(aircraft_position_?"{\"lat\":"+number(aircraft_position_->lat)+",\"lon\":"+number(aircraft_position_->lon)+"}":"null")+",\"product_version\":"+quote(IOS_PRODUCT_VERSION)+",\"weight_service\":"+weight_service_json_+",\"non_normal\":"+non_normal_json_+",\"environment\":" + environment_json_ + ",\"psx_endpoint\":" +
         quote(config_.psx + ":" + std::to_string(config_.main_port)) +
         ",\"listeners\":" + quote(listeners_) +
         ",\"web_notice\":" + quote(web_error_) +
         ",\"persistence_error\":" + quote(persistence_error_) +
         ",\"confirmation\":" + (config_.confirmation ? "true" : "false") +
         ",\"database\":{\"identity\":" + quote(catalogue_ ? catalogue_->identity : "") +
         ",\"revision\":" + std::to_string(database_revision_) +
         ",\"confirmed\":" + (catalogue_ && !active_database_.folder.empty() ? "true" : "false") +
         ",\"name\":" + quote(active_database_.name) + ",\"notice\":" + quote(database_error_) + "}}";
}
std::string Runtime::operation(std::uint64_t id) const {
  std::lock_guard lock(mutex_);
  auto o = engine_.find(id);
  if (!o)
    throw std::out_of_range("Operation not retained");
  return Engine::operation(*o);
}
std::uint64_t Runtime::submit(std::string k, bool v, std::uint64_t r,
                              std::string id) {
  std::lock_guard lock(mutex_);
  auto n = engine_.submit(std::move(k), v, r, std::move(id));
  cv_.notify_all();
  return n;
}
Profile Runtime::profile(std::string_view ip) const {
  auto key = normalize_ip(ip);
  std::lock_guard lock(mutex_);
  auto i = profiles_.find(key);
  return i == profiles_.end() ? Profile{} : i->second;
}
void Runtime::profile(std::string_view ip, const Profile &p) {
  if (!valid_tab(p.tab) || !valid_theme(p.theme) || !valid_weather_units(p.weather_units))
    throw std::invalid_argument("Invalid selected tab");
  auto key = normalize_ip(ip);
  {
    std::lock_guard lock(mutex_);
    if (!profiles_.contains(key) && profiles_.size() == 1024)
      throw std::runtime_error("Client profile capacity reached");
    auto i = profiles_.find(key);
    if (i != profiles_.end() && i->second.tab == p.tab &&
        i->second.collapsed == p.collapsed && i->second.ias == p.ias &&
        i->second.favorites == p.favorites && i->second.recent == p.recent && i->second.theme == p.theme &&
        i->second.weather_units == p.weather_units)
      return;
    profiles_[key] = p;
    ++persist_revision_;
  }
  cv_.notify_all();
}
void Runtime::mutate_profile(std::string_view ip, const std::function<void(Profile &)> &fn) {
  auto key = normalize_ip(ip);
  {
    std::lock_guard lock(mutex_);
    auto i = profiles_.find(key);
    if (i == profiles_.end() && profiles_.size() >= 1024)
      throw std::runtime_error("Client profile capacity reached");
    auto p = i == profiles_.end() ? Profile{} : i->second;
    fn(p);
    profiles_[key] = std::move(p);
    ++persist_revision_;
  }
  cv_.notify_all();
}
void Runtime::profile_ui(std::string_view ip, std::string tab, bool collapsed) {
  if (!valid_tab(tab)) throw std::invalid_argument("Invalid selected tab");
  mutate_profile(ip, [&](Profile &p) { p.tab = std::move(tab); p.collapsed = collapsed; });
}
void Runtime::profile_options(std::string_view ip, std::optional<std::string> theme,
                              std::optional<std::string> weather_units) {
  if (theme && !valid_theme(*theme)) throw std::invalid_argument("Invalid theme");
  if (weather_units && !valid_weather_units(*weather_units))
    throw std::invalid_argument("Invalid weather units");
  mutate_profile(ip, [&](Profile &p) {
    if (theme) p.theme = std::move(*theme);
    if (weather_units) p.weather_units = std::move(*weather_units);
  });
}
std::string Runtime::profile_json(std::string_view ip) const {
  auto p = profile(ip);
  std::shared_ptr<const Catalogue> catalogue;{std::lock_guard lock(mutex_);catalogue=catalogue_;}
  auto available=[&](const std::vector<std::string> &ids){std::string s="[";unsigned n{};if(catalogue)for(const auto &id:ids){auto a=catalogue->airports.find(id);if(a==catalogue->airports.end()||!a->second.eligible())continue;if(n++)s+=',';s+="{\"icao\":"+quote(id)+",\"name\":"+quote(a->second.name)+"}";}return s+"]";};
  auto array = [](const std::vector<std::string> &v) {
    std::string s = "[";
    for (std::size_t n = 0; n < v.size(); ++n) { if (n) s += ','; s += quote(v[n]); }
    return s + "]";
  };
  return "{\"selected_tab\":" + quote(p.tab) +
         ",\"status_collapsed\":" + (p.collapsed ? "true" : "false") +
         ",\"ias_kt\":" + number(p.ias) + ",\"favorites\":" + array(p.favorites) +
         ",\"recent\":" + array(p.recent) + ",\"theme\":" + quote(p.theme) +
         ",\"weather_units\":" + quote(p.weather_units) +
         ",\"favorite_airports\":"+available(p.favorites)+",\"recent_airports\":"+available(p.recent)+"}";
}
std::string Runtime::options() const {
  std::lock_guard lock(mutex_);
  return "{\"confirmation\":" + std::string(config_.confirmation ? "true" : "false") +
         ",\"database_folder\":" + quote(narrow(active_database_.folder.wstring())) +
         ",\"database_name\":" + quote(active_database_.name) + ",\"database_notice\":" + quote(database_error_) + "}";
}
void Runtime::options(bool confirmation) {
  {
    std::lock_guard lock(mutex_);
    auto c = config_; c.confirmation = confirmation;
    if (c == config_) return;
    config_ = std::move(c); ++persist_revision_;
  }
  cv_.notify_all();
}
std::string Runtime::airport_search(std::string_view s,bool center_nearest) const {
  std::shared_ptr<const Catalogue> c;
  std::optional<Point> center;
  { std::lock_guard lock(mutex_); c = catalogue_;if(center_nearest)center=aircraft_position_; }
  if (!c) throw std::runtime_error("Active PSX navigation database unavailable");
  return c->search(s,center);
}
std::string Runtime::airport_detail(std::string_view s) const {
  std::shared_ptr<const Catalogue> c;
  { std::lock_guard lock(mutex_); c = catalogue_; }
  if (!c) throw std::runtime_error("Airport database unavailable");
  return c->detail(s);
}
void Runtime::airport_preference(std::string_view ip, std::string id, std::string action, bool selected) {
  id = airport_id(id);
  if (selected) airport_detail(id);
  mutate_profile(ip, [&](Profile &p) {
  if (action == "favorite") {
    auto i = std::find(p.favorites.begin(), p.favorites.end(), id);
    if (!selected) { if (i != p.favorites.end()) p.favorites.erase(i); }
    else if (i == p.favorites.end()) {
      if (p.favorites.size() == 32) throw std::runtime_error("Favorite capacity is 32 airports");
      p.favorites.push_back(id);
    }
  } else if (action == "recent" && selected) {
    std::erase(p.recent, id); p.recent.insert(p.recent.begin(), id);
    if (p.recent.size() > 20) p.recent.pop_back();
  } else throw std::invalid_argument("Unknown airport preference action");
  });
}
void Runtime::ias_preference(std::string_view ip, double ias) {
  if (!std::isfinite(ias) || ias <= 0 || ias > 1000) throw std::invalid_argument("Invalid IAS");
  mutate_profile(ip, [&](Profile &p) { p.ias = ias; });
}
std::string Runtime::preview(std::string_view ip, const Object &o) {
  keys(o, {"icao", "runway", "preset", "ias_kt", "manual", "height_ft", "dataset", "expected_revision"});
  auto peer = normalize_ip(ip);
  auto dataset = field<std::string>(o, "dataset");
  auto revision = field<std::uint64_t>(o, "expected_revision");
  std::shared_ptr<const Catalogue> c;
  std::uint64_t dbrev{};
  { std::lock_guard lock(mutex_); c = catalogue_; dbrev = database_revision_;
    if (!c || c->identity != dataset) throw std::runtime_error("Airport database changed; select the airport and runway again");
    if (engine_.revision != revision) throw std::runtime_error("Simulator state changed; calculate a fresh preview");
  }
  auto t = target(*c, field<std::string>(o, "icao"), field<std::string>(o, "runway"), field<std::string>(o, "preset"),
                  numeric(o, "ias_kt"), field<bool>(o, "manual"), numeric(o, "height_ft"));
  std::lock_guard lock(mutex_);
  if (dbrev != database_revision_ || revision != engine_.revision)
    throw std::runtime_error("Reposition preconditions changed; calculate a fresh preview");
  auto time = now();
  std::erase_if(previews_, [&](const Preview &p) { return p.expires <= time; });
  if (previews_.size() >= 128) throw std::runtime_error("Reposition preview capacity reached");
  auto id = std::to_string(next_preview_++);
  previews_.push_back({id, std::move(peer), std::move(dataset), t, time + 30000, revision, dbrev});
  auto s = t.json(); s.pop_back();
  return s + ",\"target_id\":" + quote(id) + ",\"expires_in_ms\":30000,\"expected_revision\":" + std::to_string(revision) +
    ",\"database_revision\":" + std::to_string(dbrev) + ",\"confirmation\":" + (config_.confirmation ? "true" : "false") +
    ",\"executable\":" + (engine_.ready() ? "true" : "false") + ",\"reason\":" + quote(engine_.ready() ? "" : "PSX is disconnected") + ",\"full_pause\":true,\"preserve_motion_freeze\":true}";
}
std::string Runtime::logo() const { std::lock_guard lock(mutex_); return logo_; }
std::string Runtime::non_normal_snapshot(std::string_view kind) const {
  std::lock_guard lock(mutex_);
  if(kind=="failures") {auto s=failures_json_;s.pop_back();return s+",\"reset_all\":"+reset_json_+"}";}
  if(kind=="weather-events")return events_json_;
  if(kind=="tcas")return tcas_json_;
  if(kind=="runway-aircraft")return actor_json_;
  throw std::invalid_argument("Unknown non-normal snapshot");
}
std::string Runtime::failures_search(std::string_view text) const {
  auto ids=psx::instructor::search_failures(text);std::string out="{\"ids\":[";bool comma=false;
  for(auto id:ids){if(comma)out+=',';comma=true;out+=std::to_string(id);}return out+"]}";
}
std::string Runtime::weight_service_snapshot() const {std::lock_guard lock(mutex_);return weight_service_json_;}
std::uint64_t Runtime::fmc_command(std::string_view kind,const Object& o) {
  if(kind!="fmc-save"&&kind!="fmc-recall")throw std::invalid_argument("Invalid FMC action");
  keys(o,{"request_id","expected_revision"});
  auto request=field<std::string>(o,"request_id");auto revision=field<std::uint64_t>(o,"expected_revision");
  std::lock_guard lock(mutex_);
  return engine_.command(std::string(kind),o,revision,std::move(request),json_text(o));
}
std::string Runtime::situation_catalogue() const {return situations_.catalogue();}
std::uint64_t Runtime::situation_command(std::string_view kind,const Object& input){
  validate_situation_command(kind,input);
  std::lock_guard lock(mutex_);
  const auto id=engine_.command(std::string(kind),input,field<std::uint64_t>(input,"expected_revision"),field<std::string>(input,"request_id"),json_text(input));cv_.notify_all();return id;
}
void Runtime::cancel_situation(const Object& input){keys(input,{"operation"});situations_.cancel(field<std::uint64_t>(input,"operation"));}
std::uint64_t Runtime::weight_service(std::string_view kind,const Object &o) {
  validate_weight_service(kind,o);auto fingerprint=json_text(o);std::lock_guard lock(mutex_);
  auto id=engine_.command(std::string(kind),o,field<std::uint64_t>(o,"expected_revision"),field<std::string>(o,"request_id"),std::move(fingerprint));cv_.notify_all();return id;
}
std::string Runtime::motor_status()const{std::lock_guard lock(mutex_);return motor_status_json_;}
std::string Runtime::actuator_status()const{std::lock_guard lock(mutex_);return actuator_status_json_;}
std::uint64_t Runtime::actuator_command(std::string_view kind,const Object& input){
  for(const auto& [key,value]:input){(void)value;if(key!="kind" && key!="index" && key!="session" && key!="fault_revision" && key!="expected_revision" && key!="request_id")throw std::invalid_argument("Unknown actuator field");}
  const auto type=field<std::string>(input,"kind");const unsigned category=type=="motor"?0:type=="servo"?1:type=="stepper"?2:3;const auto index=field<std::uint64_t>(input,"index");if(category>2 || index>=20)throw std::invalid_argument("Invalid typed actuator identity");
  (void)field<std::uint64_t>(input,"session");(void)field<std::uint64_t>(input,"fault_revision");(void)field<std::uint64_t>(input,"expected_revision");auto request=input;request.erase("kind");request.erase("index");request["motor"]=std::uint64_t(category*20+unsigned(index));
  std::lock_guard lock(mutex_);const auto id=engine_.motor_command(std::string(kind),request,field<std::string>(input,"request_id"),std::string(kind)+json_text(input));cv_.notify_all();return id;
}
std::uint64_t Runtime::motor_command(std::string_view kind,const Object& o){
  for(const auto& [key,v]:o){(void)v;if(key!="motor" && key!="session" && key!="fault_revision" && key!="expected_revision" && key!="request_id")throw std::invalid_argument("Unknown motor field");}
  if(field<std::uint64_t>(o,"motor")>=motor_slots)throw std::invalid_argument("Invalid motor index");
  (void)field<std::uint64_t>(o,"session");(void)field<std::uint64_t>(o,"fault_revision");(void)field<std::uint64_t>(o,"expected_revision");
  std::lock_guard lock(mutex_);auto id=engine_.motor_command(std::string(kind),o,field<std::string>(o,"request_id"),std::string(kind)+json_text(o));cv_.notify_all();return id;
}
std::uint64_t Runtime::non_normal(std::string_view kind,const Object &o) {
  validate_non_normal(kind,o);
  std::string fingerprint=std::string(kind)+json_text(o);
  std::lock_guard lock(mutex_);
  auto id=engine_.command(std::string(kind),o,field<std::uint64_t>(o,"expected_revision"),field<std::string>(o,"request_id"),std::move(fingerprint));cv_.notify_all();return id;
}
std::uint64_t Runtime::environment(std::string_view kind,const Object &o) {
  using namespace psx::instructor;
  std::lock_guard lock(mutex_);
  auto request=field<std::string>(o,"request_id");
  std::string fingerprint=std::string(kind)+json_text(o);
  for(auto &op:engine_.operations) if(op.request==request) {
    if(op.fingerprint!=fingerprint) throw std::runtime_error("Request ID conflict"); return op.id;
  }
  EnvironmentRequest r;
  if(kind=="events") {
    for(const auto &[key,v]:o) { (void)v;if(key!="request_id"&&key!="expected_revision"&&key!="shear"&&key!="shear_top_ft"&&key!="ash")throw std::invalid_argument("Unknown weather event field"); }
    r.kind=EnvironmentRequest::Kind::events;r.expected_revision=field<std::uint64_t>(o,"expected_revision");
    if(o.contains("shear")) {
      const auto mode=field<std::string>(o,"shear");r.events.shear_mode=mode=="off"?0:mode=="left"?1:mode=="ahead"?2:mode=="right"?3:-1;
      const auto height=numeric(o,"shear_top_ft");if(std::trunc(height)!=height||height<100||height>1300||int(height)%100||*r.events.shear_mode<0)throw std::invalid_argument("Invalid windshear mode or top");
      r.events.shear_top_ft=int(height);
    } else if(o.contains("shear_top_ft"))throw std::invalid_argument("Windshear top requires a mode");
    if(o.contains("ash"))r.events.ash=field<bool>(o,"ash");
    if(!r.events.shear_mode&&!r.events.ash)throw std::invalid_argument("No weather event selected");
  } else if(kind=="weather") {
    auto target=field<std::string>(o,"target");
    if(target!="planet"&&target!="zone1"&&target!="all") throw std::invalid_argument("Invalid weather target");
    // IOS weather is global even for older browser clients sending a selected zone.
    r.target=WeatherTarget::all;
    if(o.contains("preset")) r.preset=field<std::string>(o,"preset");
    for(auto &[key,value]:o) {
      if(key=="request_id"||key=="target"||key=="preset") continue;
      auto it=std::find(std::begin(weather_names),std::end(weather_names),key);
      if(it==std::end(weather_names)) throw std::invalid_argument("Unknown weather field: "+key);
      if(!r.preset.empty()) throw std::invalid_argument("Preset and explicit fields cannot be combined");
      const auto f=static_cast<WeatherField>(it-std::begin(weather_names));auto v=numeric(o,key.c_str());
      if(f==WeatherField::visibility){if(!std::isfinite(v)||v<0||v>40000||std::trunc(v)!=v)throw std::invalid_argument("Visibility must be an integer from 0 to 40,000 m");r.commanded_visibility_m=int(v);v=std::min(v,9999.);}
      r.edits.push_back({f,v});
    }
  } else if(kind=="nearest") {
    keys(o,{"request_id"}); r.kind=EnvironmentRequest::Kind::nearest;
    if(!stations_) throw std::runtime_error("Nearest weather station unavailable: "+stations_error_);
  } else if(kind=="time") {
    for(auto &[key,value]:o) { (void)value; if(key!="request_id"&&key!="action"&&key!="hour"&&key!="minute"&&key!="offset") throw std::invalid_argument("Unknown time field"); }
    if(!civil_time_||!aircraft_position_||!earth_time_) throw std::runtime_error("Aircraft local civil time unavailable: "+time_error_);
    auto action=field<std::string>(o,"action"); auto p=*aircraft_position_; auto d=civil_time_->local(p,*earth_time_).date;
    r.kind=EnvironmentRequest::Kind::time;
    if(action=="dawn"||action=="dusk") {
      auto event=civil_time_->solar(p,d,action=="dusk"); if(!event) throw std::runtime_error("No solar event on this date at the aircraft location");
      r.utc_ms=*event-(action=="dawn"?1800000:0);
    } else {
      if(action=="clock") {
        double hour=numeric(o,"hour"), minute=numeric(o,"minute");
        if(std::trunc(hour)!=hour||std::trunc(minute)!=minute||hour<0||hour>23||minute<0||minute>59) throw std::invalid_argument("Invalid HH/MM");
        d.hour=int(hour); d.minute=int(minute);
      } else if(action=="day"||action=="night") { d.hour=action=="day"?11:1; d.minute=0; }
      else {
        int month=action=="spring"?4:action=="summer"?7:action=="autumn"?10:action=="winter"?1:0;
        if(!month) throw std::invalid_argument("Unknown time action");
        if(p.lat<0) month=(month+5)%12+1; d.month=month; d.day=15;
      }
      auto times=civil_time_->utc(p,d);
      if(times.empty()) throw std::runtime_error("This local time does not exist because of daylight saving");
      if(times.size()>1 && !o.contains("offset")) {
        std::string error="Repeated local time. Choose UTC offset seconds:";
        for(auto t:times) error+=' '+std::to_string(civil_time_->local(p,t).offset);
        throw std::runtime_error(error);
      }
      r.utc_ms=times.front();
      if(o.contains("offset")) {
        auto offset=numeric(o,"offset"); auto it=std::find_if(times.begin(),times.end(),[&](auto t){return civil_time_->local(p,t).offset==offset;});
        if(it==times.end()) throw std::invalid_argument("Invalid civil-time offset choice"); r.utc_ms=*it;
      }
    }
  } else throw std::invalid_argument("Unknown environment action");
  auto id=engine_.environment(std::move(r),std::move(request),std::move(fingerprint)); cv_.notify_all(); return id;
}
std::uint64_t Runtime::execute(std::string_view ip, const Object &o) {
  keys(o, {"target_id", "expected_revision", "request_id"});
  auto id = field<std::string>(o, "target_id"), request = field<std::string>(o, "request_id");
  auto rev = field<std::uint64_t>(o, "expected_revision");
  if (id.empty() || id.size() > 32 || request.empty() || request.size() > 96)
    throw std::invalid_argument("Invalid target or request ID");
  auto peer = normalize_ip(ip);
  std::lock_guard lock(mutex_);
  for (const auto &op : engine_.operations)
    if (op.request == request) {
      if (!op.move || op.move->preview != std::string(peer) + ":" + id || op.revision != rev)
        throw std::runtime_error("Request ID conflict");
      return op.id;
    }
  auto p = std::find_if(previews_.begin(), previews_.end(), [&](const auto &v) { return v.id == id && v.peer == peer; });
  if (p == previews_.end() || p->expires <= now()) throw std::runtime_error("Target expired or invalidated; calculate a fresh preview");
  if (rev != p->revision || rev != engine_.revision || p->database_revision != database_revision_ || !catalogue_ || p->dataset != catalogue_->identity)
    throw std::runtime_error("Reposition preconditions changed; calculate a fresh preview");
  if (!engine_.ready()) throw std::runtime_error("PSX connections are not synchronized");
  const auto &t = p->target;
  Operation::Move move;
  move.preview = std::string(peer) + ":" + id;
  move.target_id=id;move.dataset=p->dataset;move.database_revision=database_revision_;
  move.writer_generation=engine_.channels[0].generation;move.observer_generation=engine_.channels[1].generation;
  const auto identity=psx_process(active_database_);move.psx_pid=identity.pid;move.psx_birth=identity.creation_filetime;
  move.lat = t.position.lat; move.lon = t.position.lon; move.heading = t.heading;
  move.altitude = t.altitude; move.pitch = t.ground ? 0.0 : t.pitch_deg * 0.017453292519943295;
  move.ias = t.ias; move.terrain = t.threshold_elevation; move.ground = t.ground;
  return engine_.reposition(std::move(move), rev, std::move(request));
}
std::uint64_t Runtime::confirm_terrain(std::string_view ip,const Object &o) {
  keys(o,{"operation_id","target_id","request_id","continue"});
  const auto id=field<std::uint64_t>(o,"operation_id");const auto target=field<std::string>(o,"target_id");
  const auto request=field<std::string>(o,"request_id");const bool proceed=field<bool>(o,"continue");
  if(request.empty()||request.size()>64||!std::all_of(request.begin(),request.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';}))throw std::invalid_argument("Invalid request ID");
  const auto peer=normalize_ip(ip);std::lock_guard lock(mutex_);auto* op=engine_.find(id);
  if(!op||!op->move||op->move->preview!=peer+":"+target||op->move->target_id!=target)throw std::runtime_error("Reposition target changed or unavailable");
  for(const auto &other:engine_.operations)if(other.request==request||(other.id!=id&&other.terrain.request==request))throw std::runtime_error("Request ID conflict");
  // The first decision remains attached to this immutable target. Duplicate
  // replies use the retained operation; they never enqueue another placement.
  TerrainPreflight::decide(*op,proceed,request,now());cv_.notify_all();return id;
}
void Runtime::database() {
  try {
    auto time=std::make_shared<psx::instructor::CivilTime>(psx::instructor::CivilTime::load(time_data_));
    std::lock_guard lock(mutex_); civil_time_=std::move(time);
  } catch(const std::exception &e) { std::lock_guard lock(mutex_); time_error_=e.what(); }
  psx::instructor::NavigationSelection selected;
  std::string last_host;
  std::uint16_t last_port{};
  std::array<std::filesystem::file_time_type, 3> stamps{};
  bool attempted = false;
  try {
    auto path = directory_ / "customer.png";
    if (std::filesystem::exists(path)) {
      auto bytes = read(path);
      if (bytes.size() < 24 || bytes.substr(0, 8) != std::string("\x89PNG\r\n\x1a\n", 8) || bytes.substr(12, 4) != "IHDR")
        throw std::runtime_error("customer.png is not a PNG image");
      auto dimension = [&](unsigned offset) { std::uint32_t n{}; for (unsigned k = 0; k < 4; ++k) n = (n << 8) | static_cast<unsigned char>(bytes[offset + k]); return n; };
      if (!dimension(16) || !dimension(20) || dimension(16) > 4096 || dimension(20) > 4096)
        throw std::runtime_error("Customer logo dimensions exceed 4096 pixels");
      std::lock_guard lock(mutex_); logo_ = std::move(bytes);
    }
  } catch (const std::exception &e) { std::lock_guard lock(mutex_); database_error_ = e.what(); }
  for (;;) {
    Config c; { std::lock_guard lock(mutex_); if (ending_) return; c = config_; }
    std::array<std::filesystem::file_time_type, 3> times{};
    std::string error;
    psx::instructor::NavigationSelection candidate;
    try {
      candidate = database_selection_.read(c.psx, c.main_port);
      unsigned n{}; for (auto name : {"pa.w", "pg.w", "pi.w"}) times[n++] = std::filesystem::last_write_time(candidate.folder / name);
    } catch (const std::exception &e) { error = e.what(); }
    if (!attempted || selected != candidate || times != stamps || last_host != c.psx || last_port != c.main_port) {
      attempted = true; selected = candidate; stamps = times; last_host = c.psx; last_port = c.main_port;
      // Clear the old selections before loading the replacement catalogue.
      { std::lock_guard lock(mutex_);
        if (config_.psx != c.psx || config_.main_port != c.main_port) { attempted = false; continue; }
        catalogue_.reset(); stations_.reset(); active_database_ = candidate;
        previews_.clear(); ++database_revision_; database_error_ = error.empty() ? "Loading active PSX database" : error; }
      std::shared_ptr<const Catalogue> loaded;
      std::shared_ptr<const psx::instructor::WeatherStations> stations;
      std::string station_error;
      try {
        if (error.empty()) loaded = Catalogue::load(candidate.folder);
        if (error.empty()) error = "Active PSX database: " + candidate.name;
      } catch (const std::exception &e) { error = e.what(); }
      try {
        auto root=candidate.folder;
        while(!root.empty() && root!=root.root_path() && !std::filesystem::exists(root/"Aerowinx.jar")) root=root.parent_path();
        if(root.empty() || !std::filesystem::exists(root/"Aerowinx.jar")) throw std::runtime_error("PSX installation root unavailable");
        stations=std::make_shared<psx::instructor::WeatherStations>(psx::instructor::WeatherStations::load(root));
      } catch(const std::exception &e) { station_error=e.what(); }
      std::lock_guard lock(mutex_);
      if (config_.psx == c.psx && config_.main_port == c.main_port) {
        catalogue_ = std::move(loaded); stations_=std::move(stations); stations_error_=station_error; database_error_ = error; ++database_revision_;
      }
    } else {
      std::lock_guard lock(mutex_);
      if (!error.empty()) database_error_ = error;
    }
    std::optional<psx::instructor::FlapSpeeds> flap;
    std::optional<psx::instructor::InstructorSample> instructor;
    std::string instructor_error;
    std::string performance_error;
    try { const auto time=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();flap=performance_selection_.read(candidate,std::uint64_t(time)); }
    catch(const std::exception &e){performance_error=e.what();}
    try {const auto time=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();instructor=instructor_metadata_.read(candidate,std::uint64_t(time));}
    catch(const std::exception &e){instructor_error=e.what();}
    std::unique_lock lock(mutex_);
    if(config_.psx==c.psx&&config_.main_port==c.main_port){flap_speeds_=flap;flap_speed_notice_=performance_error;
      if(instructor&&instructor->sampled_unix_ms<instructor_min_time_){instructor.reset();instructor_error="Waiting for a new native sample after PSX loading";}
      instructor_sample_=instructor;instructor_notice_=instructor_error;
    }
    cv_.wait_for(lock, std::chrono::seconds(1), [&] { return ending_ || config_.psx != c.psx || config_.main_port != c.main_port; });
    if (ending_) return;
  }
}
bool Runtime::flush(unsigned ms) {
  std::unique_lock lock(mutex_);
  auto target = persist_revision_;
  cv_.notify_all();
  return cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
    return saved_revision_ >= target || !persistence_error_.empty();
  }) && saved_revision_ >= target;
}
void Runtime::persist() {
  for (;;) {
    Config c;
    std::map<std::string, Profile> p;
    std::uint64_t rev{};
    bool last{};
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock,
               [&] { return ending_ || persist_revision_ > saved_revision_; });
      last = ending_;
      rev = persist_revision_;
      if (rev == saved_revision_)
        return;
      c = config_;
      p = profiles_;
    }
    std::string error;
    try {
      save_text(directory_ / "settings.cfg", config_text(c));
      save_text(directory_ / "clients.cfg", profiles_text(p));
    } catch (const std::exception &e) {
      error = e.what();
    }
    {
      std::unique_lock lock(mutex_);
      persistence_error_ = error;
      if (error.empty())
        saved_revision_ = rev;
      cv_.notify_all();
      if (last)
        return;
      if (!error.empty())
        cv_.wait_for(lock, std::chrono::seconds(1),
                     [&] { return ending_ || persist_revision_ != rev; });
    }
  }
}
void Runtime::network() {
  struct Session {
    psx::instructor::PsxCommunicator stream;
    Tick retry{}, deadline{};
    unsigned failures{};
    bool established{};
  };
  std::array<Session, 2> sessions;
  psx::instructor::Environment environment(sessions[0].stream,sessions[1].stream);
  NonNormal non_normal(sessions[0].stream,sessions[1].stream,coordinator_endpoint_,hwio_endpoint_);
  WeightService weight(sessions[0].stream,sessions[1].stream);
  StatusReadout status_readout;
  FmcQuicksave fmc(directory_);
  HwioMotors motors;
  Visibility visibility(coordinator_endpoint_);
  std::mt19937 random{unsigned(now()) ^ GetCurrentProcessId()};
  bool manual_pending=true, subscription=false;
  std::array<bool,2> startup_clouds{true,true};
  std::string startup_cloud_notice;
  Tick snapshot_at{},controls_at{};
  auto cleanup=[&](std::string_view why) {
    ++status_trace_generation_;
    status_display_.reset();status_controls_json_="{\"ready\":false}";
    status_readout.disconnected();
    visibility.disconnected();
    weight.disconnected();
    non_normal.abort();
    non_normal.disconnected();
    environment.abort(why);
    auto deadline=now()+500;
    while(sessions[0].stream.sending() && now()<deadline) { sessions[0].stream.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    environment.disconnected();
  };
  std::uint64_t seen{}, seen_inventory{};
  bool was_enabled{};
  psx::network::Adapter previous;
  Config cfg;
  for (;;) {
    auto inv = inventory();
    psx::network::Adapter adapter;
    Tick t = now();
    bool native_ready{},visibility_ready{};psx::instructor::NavigationSelection native_selection;
    std::optional<psx::instructor::InstructorSample> instructor_sample;
    {std::lock_guard lock(mutex_);native_ready=engine_.enabled;visibility_ready=engine_.ready();native_selection=active_database_;cfg=config_;instructor_sample=instructor_sample_;}
    const auto identity=psx_process(native_selection);
    non_normal.poll(t,native_ready,identity,cfg.psx,cfg.main_port,instructor_sample,visibility_ready);
    motors.poll(t,cfg.psx,cfg.main_port);
    weight.poll(t,visibility_ready,identity);
    fmc.context(visibility_ready,identity);
    situations_.context(visibility_ready,identity,native_selection.folder);
    situations_.physical(motors.situation_controls());
    status_readout.poll(t,visibility_ready,identity);
    visibility.poll(t,visibility_ready,{identity.pid,identity.creation_filetime});
    {
      std::unique_lock lock(mutex_);
      if (ending_) {
        cleanup("IOS Exit; restoring temporary pause/freeze changes");
        break;
      }
      fmc.apply(engine_);situations_.apply(engine_);engine_.fmc_pending=fmc.busy()||situations_.busy();fmc_json_=fmc.json();
      engine_.pump_motors(t,[&](Operation& op,Tick tick,bool first){motors.command(op,tick,first);});motor_status_json_=motors.json();actuator_status_json_=motors.actuator_json();
      bool enabled = engine_.enabled;
      auto changed = seen != config_revision_ || enabled != was_enabled;
      cfg = config_;
      adapter = previous;
      const auto inventory_revision = inv ? inv->revision : 0;
      // Route queries occur only on configuration/address/route notifications,
      // never on every acquisition turn or HTTP request.
      if (seen != config_revision_ || seen_inventory != inventory_revision) {
        seen_inventory = inventory_revision;
        try {
          adapter = main_adapter(cfg, inv ? *inv : psx::network::Inventory{});
        } catch (const std::exception &e) {
          adapter = {};
          engine_.lost(0, e.what());
          engine_.lost(1, e.what());
        }
      }
      changed = changed || !psx::network::same_binding(adapter, previous);
      if (changed) {
        cleanup("IOS connection stopped/changed; restoring temporary pause/freeze changes");
        manual_pending=true; subscription=false;
        for (unsigned n = 0; n < 2; ++n) {
          sessions[n].stream.close();
          sessions[n].established = false;
          sessions[n].retry = t;
          sessions[n].failures = 0;
          engine_.lost(n, "Connection/binding changed");
        }
        previous = adapter;
        seen = config_revision_;
        was_enabled = enabled;
      }
      for (unsigned n = 0; n < 2; ++n) {
        auto &s = sessions[n];
        if (!enabled)
          continue;
        auto schedule = [&](std::string reason) {
          cleanup(reason); manual_pending=true; subscription=false;
          s.stream.close();
          s.established = false;
          engine_.lost(n, reason);
          constexpr Tick delays[]{1000, 2000, 4000, 10000};
          s.retry = t + delays[std::min(s.failures, 3u)];
          ++s.failures;
        };
        if (s.stream.state() == psx::network::State::closed && t >= s.retry) {
          s.deadline = t + 10000;
          if (!s.stream.connect(adapter, psx::network::Endpoint::parse(
                                             cfg.psx, cfg.main_port))) {
            schedule("Main connection unavailable (" +
                     std::to_string(s.stream.error()) + ")");
            continue;
          }
        }
        s.stream.pump();
        if (s.stream.state() == psx::network::State::connected &&
            !s.established) {
          s.established = true;
          engine_.connected(n);
          s.deadline = t + 10000;
        }
        s.stream.read([&](std::string_view record) {
          engine_.record(n, record, t);
          if(record=="load1") s.deadline=t+10000;
          if(record=="load3" && engine_.channels[n].connected &&
             engine_.channels[n].stage==3 && !engine_.channels[n].valid) {
            // ECON may omit an unchanged pause value during situation loading.
            // Read it once; never write a mask or replay an invalidated command.
            s.deadline=t+10000;
            s.stream.send("bang\n");
          }
          if(record=="load1"||record=="exit") {
            status_readout.disconnected();
            instructor_sample_.reset();instructor_min_time_=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            visibility.disconnected();environment.disconnected(); manual_pending=true; subscription=false;
          }
          if(n==1) {
            situations_.record(record);
            const auto history_before=status_display_.history().size();
            status_display_.record(record,t);
            if(history_before&&!status_display_.history().size())++status_trace_generation_;
            environment.record(record);
            visibility.record(record,environment.weather());
            non_normal.record(record,t);
            weight.record(record);
            if(record=="Qi139=1" || record=="load1") manual_pending=true;
            if(record=="load1") subscription=false;
          }
        });
        if (s.established && !engine_.channels[n].connected) {
          schedule(engine_.channels[n].error);
          continue;
        }
        if (s.stream.state() == psx::network::State::failed) {
          schedule("Main disconnected (" + std::to_string(s.stream.error()) +
                   ")");
          continue;
        }
        if (s.stream.state() == psx::network::State::connecting &&
            t >= s.deadline) {
          schedule("Main connect deadline");
          continue;
        }
        if (s.established && !engine_.channels[n].ready() &&
            t >= s.deadline) {
          schedule("Main synchronization deadline");
          continue;
        }
        if (engine_.channels[n].ready())
          s.failures = 0;
      }
      if(environment.busy() && engine_.idle()) environment.abort("Command invalidated; restoring temporary pause/freeze changes");
      if(environment.busy() && engine_.idle()) environment.pump(t);
      if(engine_.ready() && !subscription) { sessions[1].stream.send("demand=Qs483\ndemand=Qs481\ndemand=Qs482\ndemand=Qs479\ndemand=Qh170\ndemand=Qh389\ndemand=Qs436\n"); subscription=true; }
      if(engine_.ready() && engine_.idle() && !environment.busy() && manual_pending) {
        psx::instructor::EnvironmentRequest r; r.kind=psx::instructor::EnvironmentRequest::Kind::manual;
        engine_.environment(r,"manual-"+std::to_string(t),"automatic manual-weather enforcement"); manual_pending=false;
      }
      if(engine_.ready() && engine_.idle() && !environment.busy()) {
        for(unsigned i=0;i<startup_clouds.size();++i) {
          if(!startup_clouds[i]) continue;
          const auto target=i==0 ? psx::instructor::WeatherTarget::planet : psx::instructor::WeatherTarget::zone1;
          const auto &settings=environment.weather().settings(target);
          if(!settings) continue;
          startup_clouds[i]=false; // Once per IOS start; never replay after disconnect.
          try {
            auto edits=psx::instructor::minimum_cloud_spacing(*settings);
            if(!edits.empty()) {
              psx::instructor::EnvironmentRequest r; r.target=target; r.edits=std::move(edits);
              engine_.environment(std::move(r),"startup-clouds-"+std::to_string(i),"Startup 500 ft cloud spacing");
              break;
            }
          } catch(const std::exception &e) { startup_cloud_notice=e.what(); }
        }
      }
      if(!environment.busy() || !engine_.idle()) engine_.pump(t, [&](std::string_view line) {
        return sessions[0].stream.send(line);
      },&environment,stations_,[&](Operation &op,Tick tick,bool first) {if(op.kind=="situation-save"||op.kind=="situation-load"||op.kind=="situation-delete")situations_.command(op,first);else if(op.kind=="fmc-save"||op.kind=="fmc-recall")fmc.command(op,first);else if(op.kind=="weight-set"||op.kind=="weight-action")weight.command(op,tick,first);else non_normal.command(op,tick,first,catalogue_.get(),database_revision_);},[&](psx::instructor::EnvironmentRequest &request) {
        if(request.preset!="windy")return;
        if(!catalogue_)throw std::runtime_error("Active PSX navigation database unavailable");
        std::vector<psx::instructor::RunwayOption> runways;
        for(const auto &[id,a]:catalogue_->airports)for(const auto &r:a.runways)runways.push_back({id,r.id,r.threshold,r.heading});
        auto runway=psx::instructor::windy_runway(runways,environment.flight(),t);
        if(!runway)throw std::runtime_error("Windy runway unavailable: no FMC runway or matching runway at current position");
        request.edits=psx::instructor::windy_edits(runway->heading,std::uniform_int_distribution<int>(-6,6)(random)*10,std::uniform_int_distribution<int>(10,25)(random),std::uniform_int_distribution<int>(0,20)(random),std::uniform_int_distribution<int>(2,6)(random)*10);
        request.preset.clear();
      },[&](Operation &op) {
        if(op.environment&&op.environment->kind==psx::instructor::EnvironmentRequest::Kind::events) {
          const auto &result=environment.events().result();
          const auto status=result.status==psx::instructor::SettingsStatus::match?"matching":result.status==psx::instructor::SettingsStatus::different?"different":"unavailable";
          op.result_json="{\"configured_readback\":"+quote(status)+",\"physical_application_observed\":false}";return;
        }
        if(op.state!="observed"||!op.environment)return;
        const auto &r=*op.environment;
        if(r.kind!=psx::instructor::EnvironmentRequest::Kind::weather)return;
        const auto &settings=environment.weather().settings(r.target);
        if(!settings)return;
        const int configured=int(settings->value(psx::instructor::WeatherField::visibility));
        const bool edited=r.commanded_visibility_m.has_value()||!r.preset.empty()||std::any_of(r.edits.begin(),r.edits.end(),[](const auto &edit){return edit.field==psx::instructor::WeatherField::visibility;});
        if(edited) {
          const int commanded=r.commanded_visibility_m.value_or(configured);
          visibility.selected(r.target,commanded);
          op.result_json="{\"commanded_visibility_m\":"+std::to_string(commanded)+",\"psx_configured_visibility_m\":"+std::to_string(configured)+",\"xplane_status\":\"See environment visibility status\"}";
          if(commanded>9999)op.detail+="; PSX 9,999 m; extended X-Plane visibility requested separately";
        }
      },[&](Operation &op,Tick tick) {
        const auto &m=*op.move;
        const auto current=psx_process(active_database_);
        if(m.database_revision!=database_revision_||!catalogue_||m.dataset!=catalogue_->identity||m.writer_generation!=engine_.channels[0].generation||m.observer_generation!=engine_.channels[1].generation||m.psx_pid!=current.pid||m.psx_birth!=current.creation_filetime) {
          op.state="superseded";op.detail="Destination or PSX session changed";return false;
        }
        return terrain_.poll(op,tick);
      });
      engine_.refresh_pause(t, [&](std::string_view line) { return sessions[1].stream.send(line); });
      if(t>=snapshot_at) {
        snapshot_at=t+200;
        weight_service_json_=weight.snapshot();
        failures_json_=non_normal.failures();tcas_json_=non_normal.tcas();actor_json_=non_normal.actor();reset_json_=non_normal.reset();
        {auto &f=failures_json_;f.pop_back();f+=",\"hwio\":"+non_normal.hwio()+",\"cdu\":"+non_normal.cdu()+"}";}
        const auto &events=environment.events();const auto zone=events.focused();
        const auto &zone_settings=zone?events.settings()[*zone]:events.settings()[0];
        const auto utc=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const bool sampled=engine_.ready()&&instructor_sample_&&instructor_sample_->loading==false&&instructor_sample_->sampled_unix_ms<=utc&&utc-instructor_sample_->sampled_unix_ms<=2000;
        const auto tropopause=sampled?instructor_sample_->tropopause_ft:std::optional<double>{};
        events_json_="{\"focused_zone\":"+(zone?std::to_string(*zone):"null")+
          ",\"shear_mode\":"+(zone&&zone_settings?zone_settings->tokens()[12]:"null")+
          ",\"shear_top_ft\":"+(zone&&zone_settings?zone_settings->tokens()[14]:"null")+
          ",\"cloud_compatible\":"+std::string(zone&&zone_settings&&(zone_settings->value(psx::instructor::WeatherField::lower_coverage)>0||zone_settings->value(psx::instructor::WeatherField::cb_coverage)>0)?"true":"false")+
          ",\"ash_on\":"+(events.ash_on()?(*events.ash_on()?"true":"false"):"null")+
          ",\"tropopause_ft\":"+(tropopause?number(*tropopause):"null")+
          ",\"ash_limit_fl\":"+(tropopause?std::to_string(int(std::ceil(*tropopause*.004))):"null")+
          ",\"readout_notice\":"+quote(tropopause?"":sampled?instructor_sample_->tropopause_error:instructor_notice_.empty()?"Native tropopause sample unavailable":instructor_notice_)+"}";
        non_normal_json_="{\"states\":"+non_normal.states()+",\"hwio\":"+non_normal.hwio()+",\"cdu\":"+non_normal.cdu()+",\"reset_all\":"+reset_json_+",\"tcas\":"+tcas_json_+",\"actor\":"+actor_json_+",\"weather_events\":"+events_json_+"}";
        const auto &w=environment.weather();
        std::string s="{\"automatic\":"+std::string(w.automatic() ? (*w.automatic()?"true":"false") : "null")+
          ",\"focused_zone\":"+(w.focused_zone()?std::to_string(*w.focused_zone()):"null")+
          ",\"nearest_available\":"+(stations_?"true":"false")+",\"nearest_notice\":"+quote(stations_error_)+
          ",\"magnetic_variation\":"+(environment.variation()?number(*environment.variation()):"null");
        s+=",\"startup_notice\":"+quote(startup_cloud_notice)+",\"visibility\":"+visibility.status();
        for(auto target:{psx::instructor::WeatherTarget::planet,psx::instructor::WeatherTarget::zone1}) {
          s+=target==psx::instructor::WeatherTarget::planet ? ",\"planet\":" : ",\"zone1\":";
          auto &settings=w.settings(target); if(!settings) { s+="null"; continue; }
          s+="{"; for(unsigned i=0;i<std::size(psx::instructor::weather_names);++i) { if(i) s+=',';
            s+=quote(psx::instructor::weather_names[i])+':'+number(settings->value(static_cast<psx::instructor::WeatherField>(i))); }
          s+="}";
        }
        s+=",\"station\":"+quote(w.zone()?w.zone()->identifier.substr(0,4):"");
        s+=",\"station_elevation_ft\":"+(w.zone()?std::to_string(w.zone()->elevation):"null");
        aircraft_position_=environment.position(); earth_time_=environment.clocks()[0];
        s+=",\"time\":";
        try {
          if(!civil_time_||!aircraft_position_||!earth_time_) throw std::runtime_error(time_error_.empty()?"Waiting for aircraft position and Earth clock":time_error_);
          auto c=civil_time_->local(*aircraft_position_,*earth_time_); auto &d=c.date;
          s+="{\"year\":"+std::to_string(d.year)+",\"month\":"+std::to_string(d.month)+",\"day\":"+std::to_string(d.day)+
            ",\"hour\":"+std::to_string(d.hour)+",\"minute\":"+std::to_string(d.minute)+",\"zone\":"+quote(c.zone)+
            ",\"offset\":"+std::to_string(c.offset)+",\"utc_ms\":"+std::to_string(*earth_time_)+
            ",\"dawn_available\":"+(civil_time_->solar(*aircraft_position_,d,false)?"true":"false")+
            ",\"dusk_available\":"+(civil_time_->solar(*aircraft_position_,d,true)?"true":"false")+"}";
        } catch(const std::exception &e) { s+="null,\"time_notice\":"+quote(e.what()); }
        environment_json_=s+"}";
      }
      if(engine_.ready()) {
        const auto [aid,rid]=status_display_.flight().fmc();const auto key=aid+rid;
        if(status_trace_database_!=database_revision_||status_trace_runway_!=key){status_display_.clear_trace();++status_trace_generation_;status_trace_database_=database_revision_;status_trace_runway_=key;}
        status_display_.sample(t);
      }
      if(t>=controls_at){controls_at=t+66;const auto fast=status_readout.available();status_controls_json_=status_controls_json(fast?status_readout.values(t):status_display_.values(),engine_.ready()&&!status_readout.loading(),t,fast);}
    }
    non_normal.flush();
    weight.flush();
    // Wake on socket events, commands, or a short bounded deadline; no busy
    // spin.
    HANDLE events[]{sessions[0].stream.event(), sessions[1].stream.event()};
    const auto wait_at=now();
    WaitForMultipleObjects(2, events, FALSE, DWORD(std::min<Tick>(20,controls_at>wait_at?controls_at-wait_at:0)));
  }
  for (auto &s : sessions)
    s.stream.close();
}
} // namespace ios
