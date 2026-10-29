#include <algorithm>
#include <charconv>
#include <civetweb.h>
#include <ios/web.hpp>
#include <set>
#include <memory>
#include <fstream>
#include <array>
#include <psx/windows/runtime_dependency.hpp>
namespace ios {
namespace {
struct Site {
  Runtime &runtime;
  std::set<std::string> hosts;
  std::mutex log_mutex;
  std::string last_error;
};
// Cache the two operator-supplied font files once. Request workers borrow
// immutable bytes; they never reopen a font or depend on the working directory.
const std::array<std::string, 2>& external_fonts() {
  static const auto fonts = [] {
    std::array<std::string, 2> data;
    const auto directory = psx::windows::runtime_module_directory(reinterpret_cast<const void*>(&external_fonts));
    constexpr const wchar_t* names[]{L"B612-Regular.ttf", L"B612-Bold.ttf"};
    for (unsigned index = 0; index < data.size(); ++index) {
      const auto path = psx::windows::runtime_dependency_path(directory, names[index]);
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file) continue;
      const auto length = file.tellg();
      if (length <= 0 || length > 16 * 1024 * 1024) continue;
      auto& bytes = data[index];
      bytes.resize(static_cast<std::size_t>(length));
      file.seekg(0);
      if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) bytes.clear();
    }
    return data;
  }();
  return fonts;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
  });
  return s;
}
int reply(mg_connection *c, int status, std::string_view body,
          std::string_view type = "application/json") {
  auto reason = status == 200   ? "OK"
                : status == 202 ? "Accepted"
                : status == 404 ? "Not Found"
                : status == 405 ? "Method Not Allowed"
                : status == 409 ? "Conflict"
                : status == 413 ? "Content Too Large"
                : status == 415 ? "Unsupported Media Type"
                : status == 503 ? "Unavailable"
                : status == 403 ? "Forbidden"
                                : "Bad Request";
  mg_printf(c,
            "HTTP/1.1 %d %s\r\nContent-Type: %.*s\r\nContent-Length: "
            "%zu\r\nCache-Control: no-store\r\nConnection: "
            "close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: "
            "DENY\r\nContent-Security-Policy: default-src 'self'; script-src "
            "'self'; style-src 'self'; font-src 'self'; connect-src 'self'; "
            "frame-ancestors 'none'; base-uri 'none'\r\n\r\n",
            status, reason, int(type.size()), type.data(), body.size());
  if (!body.empty())
    mg_write(c, body.data(), body.size());
  return status;
}
int error(mg_connection *c, int n, std::string_view s) {
  return reply(c, n, "{\"error\":" + quote(s) + "}");
}
int request(mg_connection *c) {
  const auto *ri = mg_get_request_info(c);
  auto &site = *static_cast<Site *>(ri->user_data);
  try {
    std::set<std::string> singleton;
    for (int n = 0; n < ri->num_headers; ++n) {
      auto name = lower(ri->http_headers[n].name);
      if (name == "host" || name == "origin" || name == "content-length" ||
          name == "content-type" || name == "transfer-encoding")
        if (!singleton.insert(name).second)
          return error(c, 400, "Duplicate request header");
    }
    const char *h = mg_get_header(c, "Host");
    if (!h || !site.hosts.contains(lower(h)))
      return error(c, 403, "Host not published by IOS");
    if (!ri->local_uri || ri->query_string)
      return error(c, 400, "Unexpected request target");
    std::string_view route = ri->local_uri, method = ri->request_method;
    if (method == "GET") {
      if(route=="/api/v1/situations")return reply(c,200,site.runtime.situation_catalogue());
      if(route=="/api/v1/status/controls")return reply(c,200,site.runtime.status_controls());
      if(route=="/api/v1/status/summary")return reply(c,200,site.runtime.status_summary());
      if(route=="/api/v1/hwio/actuators")return reply(c,200,site.runtime.actuator_status());
      if(route=="/api/v1/hwio/motors")return reply(c,200,site.runtime.motor_status());
      if(route=="/api/v1/weight-service")return reply(c,200,site.runtime.weight_service_snapshot());
      if(route=="/api/v1/non-normal/failures")return reply(c,200,site.runtime.non_normal_snapshot("failures"));
      if(route=="/api/v1/non-normal/tcas")return reply(c,200,site.runtime.non_normal_snapshot("tcas"));
      if(route=="/api/v1/non-normal/runway-aircraft")return reply(c,200,site.runtime.non_normal_snapshot("runway-aircraft"));
      if(route=="/api/v1/non-normal/weather-events")return reply(c,200,site.runtime.non_normal_snapshot("weather-events"));
      if (route == "/api/v1/status")
        return reply(c, 200, site.runtime.status());
      if (route == "/api/v1/capabilities")
        return reply(c, 200,
                     "{\"api_version\":1,\"qualified_psx_versions\":[\"10."
                     "195\",\"10.195 NG\"],\"version_gated\":false,"
                     "\"pause\":true,\"motion_freeze\":true,\"effective_pause_"
                     "receipt\":false,\"map\":true,\"reposition_preview\":true,\"time\":true,\"weather\":"
                     "true,\"reposition\":true,\"situations\":true,\"non_normal_api\":true,\"failures\":true,\"tcas\":true}");
      if (route == "/api/v1/client-config")
        return reply(c, 200, site.runtime.profile_json(ri->remote_addr));
      if (route == "/api/v1/options")
        return reply(c, 200, site.runtime.options());
      if (route == "/customer.png") {
        auto logo = site.runtime.logo();
        return logo.empty() ? error(c, 404, "Customer logo not configured") : reply(c, 200, logo, "image/png");
      }
      constexpr std::string_view airports = "/api/v1/airports/";
      if (route.starts_with(airports)) return reply(c, 200, site.runtime.airport_detail(route.substr(airports.size())));
      constexpr std::string_view prefix = "/api/v1/operations/";
      if (route.starts_with(prefix)) {
        auto id = route.substr(prefix.size());
        std::uint64_t n{};
        auto r = std::from_chars(id.data(), id.data() + id.size(), n);
        if (id.empty() || r.ec != std::errc{} || r.ptr != id.data() + id.size())
          return error(c, 400, "Invalid operation ID");
        return reply(c, 200, site.runtime.operation(n));
      }
      if (route == "/B612-Regular.ttf" || route == "/B612-Bold.ttf") {
        const auto& font = external_fonts()[route == "/B612-Regular.ttf" ? 0 : 1];
        return font.empty() ? error(c, 404, "Place B612-Regular.ttf and B612-Bold.ttf beside IOS or one folder above")
                            : reply(c, 200, font, "font/ttf");
      }
      for (const auto &a : assets())
        if (route == a.route)
          return reply(c, 200, {reinterpret_cast<const char *>(a.data), a.size},
                       a.type);
      return error(c, 404, "Unknown route");
    }
    if (method != "POST")
      return error(c, 405, "GET or POST required");
    const auto *origin = mg_get_header(c, "Origin");
    if (!origin || lower(origin) != "http://" + lower(h))
      return error(c, 403, "Same-origin JSON write required");
    const auto *ct = mg_get_header(c, "Content-Type");
    if (!ct || (std::string_view(ct) != "application/json" &&
                std::string_view(ct) != "application/json; charset=utf-8"))
      return error(c, 415, "application/json required");
    if (mg_get_header(c, "Transfer-Encoding") || ri->content_length < 0)
      return error(c, 400, "Explicit Content-Length required");
    if (ri->content_length > 4096)
      return error(c, 413, "API body limit 4096 bytes");
    std::string body(std::size_t(ri->content_length), 0);
    std::size_t used{};
    while (used < body.size()) {
      int n = mg_read(c, body.data() + used, body.size() - used);
      if (n <= 0)
        return error(c, 400, "Incomplete request body");
      used += std::size_t(n);
    }
    auto o = json(body);
    if(route=="/api/v1/situations/save"||route=="/api/v1/situations/load"||route=="/api/v1/situations/delete"){const auto id=site.runtime.situation_command(route.ends_with("/save")?"situation-save":route.ends_with("/delete")?"situation-delete":"situation-load",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/situations/cancel"){site.runtime.cancel_situation(o);return reply(c,202,"{}");}
    if(route=="/api/v1/status/summary") {keys(o,{"airport","runway"});auto a=airport_id(field<std::string>(o,"airport"));auto r=field<std::string>(o,"runway");if(r.size()<2||r.size()>3||r[0]<'0'||r[0]>'3'||r[1]<'0'||r[1]>'9'||(r[0]=='0'&&r[1]=='0')||(r[0]=='3'&&r[1]>'6')||(r.size()==3&&r[2]!='L'&&r[2]!='C'&&r[2]!='R'))throw std::invalid_argument("Invalid runway");return reply(c,200,site.runtime.status_summary(a,r));}
    if(route=="/api/v1/hwio/actuators/reset-and-home" || route=="/api/v1/hwio/actuators/stop"){auto id=site.runtime.actuator_command(route.ends_with("/stop")?"actuator-stop":"actuator-reset-home",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/hwio/motors/reset-and-home" || route=="/api/v1/hwio/motors/stop"){auto id=site.runtime.motor_command(route.ends_with("/stop")?"motor-stop":"motor-reset-home",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/fmc/save"||route=="/api/v1/fmc/recall") {auto id=site.runtime.fmc_command(route.ends_with("/save")?"fmc-save":"fmc-recall",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/weight-service/set"||route=="/api/v1/weight-service/action") {auto id=site.runtime.weight_service(route.ends_with("/action")?"weight-action":"weight-set",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/non-normal/failures/search") {
      keys(o,{"text"});return reply(c,200,site.runtime.failures_search(field<std::string>(o,"text")));
    }
    constexpr std::pair<std::string_view,std::string_view> non_normal_routes[]{
      {"/api/v1/non-normal/failures/set","failure-set"},{"/api/v1/non-normal/failures/clear","failure-clear"},
      {"/api/v1/non-normal/failures/reset-all","failure-reset-all"},{"/api/v1/non-normal/tcas","tcas"},
      {"/api/v1/non-normal/failures/batch","failure-batch"},{"/api/v1/non-normal/reset-after-crash","crash-reset"},
      {"/api/v1/non-normal/runway-aircraft/candidates","runway-candidates"},{"/api/v1/non-normal/runway-aircraft/place","runway-place"},
      {"/api/v1/non-normal/runway-aircraft/remove","runway-remove"}};
    for(const auto &[path,kind]:non_normal_routes)if(route==path) {auto id=site.runtime.non_normal(kind,o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/non-normal/weather-events") {auto id=site.runtime.environment("events",o);return reply(c,202,site.runtime.operation(id));}
    if(route=="/api/v1/weather" || route=="/api/v1/weather/nearest" || route=="/api/v1/time") {
      auto id=site.runtime.environment(route=="/api/v1/time"?"time":route=="/api/v1/weather/nearest"?"nearest":"weather",o);
      return reply(c,202,site.runtime.operation(id));
    }
    if (route == "/api/v1/pause" || route == "/api/v1/motion-freeze") {
      keys(o, {"desired", "request_id", "expected_revision"});
      auto id = site.runtime.submit(
          route == "/api/v1/pause" ? "pause" : "motion-freeze",
          field<bool>(o, "desired"),
          field<std::uint64_t>(o, "expected_revision"),
          field<std::string>(o, "request_id"));
      return reply(c, 202, site.runtime.operation(id));
    }
    if (route == "/api/v1/connection") {
      keys(o, {"connect"});
      site.runtime.control(field<bool>(o, "connect"));
      return reply(c, 202, site.runtime.status());
    }
    if (route == "/api/v1/client-config") {
      if (o.contains("theme") || o.contains("weather_units")) {
        if (o.contains("theme") && o.contains("weather_units"))
          keys(o, {"theme", "weather_units"});
        else if (o.contains("theme"))
          keys(o, {"theme"});
        else
          keys(o, {"weather_units"});
        site.runtime.profile_options(ri->remote_addr,
          o.contains("theme") ? std::optional{field<std::string>(o, "theme")} : std::nullopt,
          o.contains("weather_units") ? std::optional{field<std::string>(o, "weather_units")} : std::nullopt);
        return reply(c, 202, site.runtime.profile_json(ri->remote_addr));
      }
      keys(o, {"selected_tab", "status_collapsed"});
      site.runtime.profile_ui(ri->remote_addr, field<std::string>(o, "selected_tab"), field<bool>(o, "status_collapsed"));
      return reply(c, 202, site.runtime.profile_json(ri->remote_addr));
    }
    if (route == "/api/v1/options") {
        keys(o, {"confirmation"});
        site.runtime.options(field<bool>(o, "confirmation"));
      return reply(c, 202, site.runtime.options());
    }
    if (route == "/api/v1/airports/search") {
        if(o.contains("center_nearest")) keys(o, {"prefix","center_nearest"});
        else keys(o, {"prefix"});
        return reply(c, 200, site.runtime.airport_search(field<std::string>(o, "prefix"),o.contains("center_nearest")?field<bool>(o,"center_nearest"):false));
    }
    if (route == "/api/v1/airport-preference") {
      keys(o, {"icao", "action", "selected"});
      site.runtime.airport_preference(ri->remote_addr, field<std::string>(o, "icao"), field<std::string>(o, "action"), field<bool>(o, "selected"));
      return reply(c, 202, site.runtime.profile_json(ri->remote_addr));
    }
    if (route == "/api/v1/ias-preference") {
      keys(o, {"ias_kt"}); site.runtime.ias_preference(ri->remote_addr, numeric(o, "ias_kt"));
      return reply(c, 202, site.runtime.profile_json(ri->remote_addr));
    }
    if (route == "/api/v1/reposition/preview")
      return reply(c, 200, site.runtime.preview(ri->remote_addr, o));
    if (route == "/api/v1/reposition/execute") {
      auto id = site.runtime.execute(ri->remote_addr, o);
      return reply(c, 202, site.runtime.operation(id));
    }
    if (route == "/api/v1/reposition/terrain-confirm") {
      const auto id=site.runtime.confirm_terrain(ri->remote_addr,o);
      return reply(c,202,site.runtime.operation(id));
    }
    return error(c, 404, "Unknown operation");
  } catch (const std::invalid_argument &e) {
    return error(c, 400, e.what());
  } catch (const std::out_of_range &e) {
    return error(c, 404, e.what());
  } catch (const std::runtime_error &e) {
    return error(c, 409, e.what());
  } catch (...) {
    return error(c, 503, "IOS request failed");
  }
}
} // namespace
namespace {
struct Listener {
  Site site;
  std::string address, authority;
  std::uint16_t port;
  mg_context *context{};
  std::uint64_t retry_after{};
  Listener(Runtime &runtime, std::string ip, std::uint16_t value)
      : site{runtime, {}, {}, {}}, address(std::move(ip)),
        authority(address + ":" + std::to_string(value)), port(value) {
    site.hosts.insert(lower(authority));
    if (address == "127.0.0.1") site.hosts.insert("localhost:" + std::to_string(port));
  }
  ~Listener() { if (context) mg_stop(context); }
  void start() {
    if (context || GetTickCount64() < retry_after) return;
    {
      std::lock_guard lock(site.log_mutex);
      site.last_error.clear();
    }
    mg_callbacks callbacks{}; callbacks.begin_request = request;
    callbacks.log_message = [](const mg_connection *connection, const char *message) {
      try {
        auto *source = connection ? static_cast<Site *>(mg_get_user_data(mg_get_context(connection))) : nullptr;
        if (source) { std::lock_guard lock(source->log_mutex); source->last_error = std::string_view(message ? message : "").substr(0, 256); }
      } catch (...) { /* Optional diagnostic logging cannot interrupt the C server. */ }
      return 1;
    };
    const char *options[]{"listening_ports",
                          authority.c_str(),
                          "num_threads",
                          "8",
                          "connection_queue",
                          "32",
                          "listen_backlog",
                          "32",
                          "max_request_size",
                          "16384",
                          "request_timeout_ms",
                          "3000",
                          "enable_keep_alive",
                          "no",
                          "enable_directory_listing",
                          "no",
                          "access_control_allow_origin",
                          "",
                          "access_control_allow_methods",
                          "",
                          "access_control_allow_headers",
                          "",
                          nullptr};
    context = mg_start(&callbacks, &site, options);
    if (!context) retry_after = GetTickCount64() + 1000;
  }
  std::string failure() {
    std::lock_guard lock(site.log_mutex);
    return "HTTP listener " + authority + " unavailable (retrying)" +
           (site.last_error.empty() ? "" : ": " + site.last_error);
  }
};
}
void Web::run() {
  mg_init_library(0);
  {
    // Each address owns its own context: LAN replacement or bind failure must
    // neither tear down loopback nor suppress another successfully bound IP.
    std::vector<std::unique_ptr<Listener>> listeners;
    while (!stopping_) {
      auto b = binding_source_ ? binding_source_() : runtime_.web_binding();
      if (std::find(b.addresses.begin(), b.addresses.end(), "127.0.0.1") == b.addresses.end())
        b.addresses.insert(b.addresses.begin(), "127.0.0.1");
      std::erase_if(listeners, [&](const auto &listener) {
        return listener->port != b.port || std::find(b.addresses.begin(), b.addresses.end(), listener->address) == b.addresses.end();
      });
      for (const auto &ip : b.addresses)
        if (std::none_of(listeners.begin(), listeners.end(), [&](const auto &listener) { return listener->address == ip; }))
          listeners.push_back(std::make_unique<Listener>(runtime_, ip, b.port));
      std::string active, notice = b.notice;
      for (auto &listener : listeners) {
        listener->start();
        if (listener->context) { if (!active.empty()) active += ','; active += listener->authority; }
        else { if (!notice.empty()) notice += "; "; notice += listener->failure(); }
      }
      runtime_.web_status(std::move(active), std::move(notice));
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  runtime_.web_status({}, "HTTP service stopped");
  mg_exit_library();
}
} // namespace ios
