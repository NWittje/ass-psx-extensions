#pragma once
#include "core.hpp"
#include "airport.hpp"
#include "situations.hpp"
#include "terrain_preflight.hpp"
#include "xplane_health.hpp"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <psx/network/io.hpp>
#include <thread>
#include <psx/instructor/navigation_selection.hpp>
#include <psx/instructor/performance_selection.hpp>
#include <psx/instructor/instructor_metadata.hpp>
#include <psx/instructor/civil_time.hpp>
#include <psx/instructor/status_display.hpp>
namespace ios {
std::string status_controls_json(const psx::instructor::StatusDisplay::Values &,bool,psx::instructor::Tick,bool native=false);
std::string status_performance_json(const std::optional<psx::instructor::FlapSpeeds> &,std::uint64_t now_unix_ms);
std::wstring wide(std::string_view);
std::string narrow(std::wstring_view);
Config read_config(const std::filesystem::path &);
std::map<std::string, Profile> read_profiles(const std::filesystem::path &);
void save_text(const std::filesystem::path &, std::string_view);
std::string config_text(const Config &);
std::string profiles_text(const std::map<std::string, Profile> &);
struct Binding {
  std::uint16_t port{};
  std::vector<std::string> addresses;
  std::string notice;
  bool operator==(const Binding &) const = default;
};
struct WebStatus {
  std::string listeners, notice;
};
Binding binding(const Config &, const psx::network::Inventory &);
class Runtime {
  std::filesystem::path directory_;
  std::filesystem::path time_data_;
  std::wstring coordinator_endpoint_;
  std::wstring hwio_endpoint_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  Config config_;
  std::map<std::string, Profile> profiles_;
  Engine engine_;
  TerrainPreflight terrain_;
  XplaneHealth xplane_health_;
  Situations situations_;
  bool ending_{};
  std::uint64_t config_revision_{1}, persist_revision_{}, saved_revision_{};
  std::string persistence_error_, web_error_, listeners_;
  std::thread network_, persistence_;
  std::thread database_;
  std::shared_ptr<const Catalogue> catalogue_;
  std::string database_error_, logo_;
  psx::instructor::NavigationDatabaseSelection database_selection_;
  psx::instructor::PerformanceSelection performance_selection_;
  psx::instructor::InstructorMetadata instructor_metadata_;
  std::optional<psx::instructor::InstructorSample> instructor_sample_;
  std::string instructor_notice_;
  std::uint64_t instructor_min_time_{};
  psx::instructor::NavigationSelection active_database_;
  std::uint64_t database_revision_{};
  std::optional<psx::instructor::FlapSpeeds> flap_speeds_;
  std::string flap_speed_notice_{"PSX flap speeds unavailable"};
  std::shared_ptr<const psx::instructor::WeatherStations> stations_;
  std::shared_ptr<const psx::instructor::CivilTime> civil_time_;
  std::string stations_error_, time_error_, environment_json_{"{}"};
  std::string failures_json_{"{\"failures\":[]}"},tcas_json_{"{\"presets\":[]}"},actor_json_{"{\"available\":false}"},reset_json_{"{\"available\":false}"};
  std::string actuator_status_json_{"{\"available\":false}"};
  std::string motor_status_json_{"{\"available\":false}"};
  std::string non_normal_json_{"{}"},events_json_{"{}"},weight_service_json_{"{}"};
  std::string fmc_json_{"{\"save_available\":false,\"recall_available\":false,\"saved_file\":false,\"busy\":false}"};
  std::optional<psx::instructor::Point> aircraft_position_;
  std::optional<std::int64_t> earth_time_;
  psx::instructor::StatusDisplay status_display_;
  std::string status_controls_json_{"{\"ready\":false}"};
  std::uint64_t status_trace_database_{};
  std::uint64_t status_trace_generation_{1};
  std::string status_trace_runway_;
  struct Preview {
    std::string id, peer, dataset;
    Target target;
    Tick expires{};
    std::uint64_t revision{}, database_revision{};
  };
  std::deque<Preview> previews_;
  std::uint64_t next_preview_{1};
  void mutate_profile(std::string_view, const std::function<void(Profile &)> &);
  psx::network::AdapterMonitor inventory_;
  void network();
  void persist();
  void database();

public:
  explicit Runtime(std::filesystem::path, std::filesystem::path navigation_metadata = {}, std::filesystem::path time_data = {}, std::wstring coordinator_endpoint = {},std::wstring hwio_endpoint = {}, TerrainPreflight::Check terrain_check = {});
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Config config() const;
  void configure(const Config &);
  std::shared_ptr<const psx::network::Inventory> inventory() const {
    return inventory_.snapshot();
  }
  Binding web_binding() const;
  void web_status(std::string, std::string);
  WebStatus web_status() const;
  std::string status() const;
  std::string status_controls() const;
  std::string xplane_status() const { return xplane_health_.snapshot(); }
  std::string status_summary(std::string_view airport = {}, std::string_view runway = {}) const;
  std::string operation(std::uint64_t) const;
  std::uint64_t submit(std::string, bool, std::uint64_t, std::string);
  void control(bool);
  Profile profile(std::string_view) const;
  void profile(std::string_view, const Profile &);
  void profile_ui(std::string_view, std::string, bool);
  void profile_options(std::string_view, std::optional<std::string> theme,
                       std::optional<std::string> weather_units);
  std::string profile_json(std::string_view) const;
  std::string options() const;
  void options(bool confirmation);
  std::string airport_search(std::string_view, bool center_nearest = false) const;
  std::string airport_detail(std::string_view) const;
  void airport_preference(std::string_view ip, std::string id, std::string action, bool selected);
  void ias_preference(std::string_view ip, double);
  std::string preview(std::string_view ip, const Object &);
  std::uint64_t execute(std::string_view ip, const Object &);
  std::uint64_t confirm_terrain(std::string_view ip, const Object &);
  std::uint64_t environment(std::string_view kind, const Object &);
  std::uint64_t non_normal(std::string_view kind,const Object &);
  std::uint64_t weight_service(std::string_view kind,const Object &);
  std::uint64_t fmc_command(std::string_view kind,const Object &);
  std::uint64_t situation_command(std::string_view kind,const Object &);
  std::string situation_catalogue() const;
  void cancel_situation(const Object&);
  std::string weight_service_snapshot() const;
  std::string motor_status() const;
  std::string actuator_status()const;
  std::uint64_t actuator_command(std::string_view,const Object&);
  std::uint64_t motor_command(std::string_view,const Object&);
  std::string non_normal_snapshot(std::string_view kind) const;
  std::string failures_search(std::string_view text) const;
  std::string logo() const;
  bool flush(unsigned milliseconds = 3000);
};
} // namespace ios
