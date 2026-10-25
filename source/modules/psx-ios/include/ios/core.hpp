#pragma once
#include <psx/instructor/reposition.hpp>
#include <psx/instructor/environment.hpp>
#include <array>
#include <algorithm>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace ios {
inline constexpr unsigned motor_slots=20; // Stable IOS motor IDs; a legacy provider exposes fewer.
using Tick = std::uint64_t; // monotonic milliseconds
Tick now();
std::string quote(std::string_view);
using Scalar = std::variant<std::string, bool, std::uint64_t, double>;
using ArrayItem = std::map<std::string, Scalar>;
using Array = std::vector<ArrayItem>; // Only bounded arrays of flat typed objects, no arbitrary nesting.
using Value = std::variant<std::string, bool, std::uint64_t, double, Array>;
using Object = std::map<std::string, Value>;
Object json(std::string_view);
std::string json_text(const Object &);
Object object(const ArrayItem &);
double numeric(const Object &, const char *);
void keys(const Object &, std::initializer_list<const char *>);
template <class T> T field(const Object &o, const char *k) {
  auto i = o.find(k);
  if (i == o.end() || !std::holds_alternative<T>(i->second))
    throw std::invalid_argument(std::string("Invalid field: ") + k);
  return std::get<T>(i->second);
}
using Framer = psx::instructor::Framer;
struct Channel {
  std::uint64_t generation{};
  bool connected{}, identified{}, versioned{}, valid{};
  int stage{}, mask{};
  std::optional<int> engine_model; // Qi1 family from the independent Main observer.
  Tick observed{};
  std::string version, error;
  bool ready() const {
    return connected && identified && versioned && valid && stage == 3;
  }
};
struct Operation {
  std::uint64_t id{}, revision{};
  std::string request, kind, state{"queued"}, detail;
  bool desired{};
  int target{-1};
  Tick sent{};
  struct Move : psx::instructor::RepositionTarget {
    std::string preview, target_id, dataset;
    std::uint64_t database_revision{}, writer_generation{}, observer_generation{};
    std::uint32_t psx_pid{};
    std::uint64_t psx_birth{};
  };
  std::optional<Move> move;
  struct Terrain {
    std::string verdict, decision, request;
    Tick deadline{};
  } terrain;
  std::optional<psx::instructor::EnvironmentRequest> environment;
  std::optional<Object> command;
  std::string result_json{"null"};
  std::string fingerprint;
  bool terminal() const { return state != "queued" && state != "sent" && state != "checking" && state != "awaiting-confirmation"; }
};
class Engine {
  std::deque<std::uint64_t> queue_,motor_queue_;
  std::array<std::uint64_t,60> motor_active_{};
  std::uint64_t next_{1};
  std::optional<std::uint64_t> active_;
  bool pause_refresh_requested_{}, pause_refresh_waiting_{};
  Tick pause_refresh_deadline_{};
  std::uint64_t pause_refresh_operation_{};
  psx::instructor::Reposition relocation_;
  void invalidate(std::string_view);

public:
  std::array<Channel, 2> channels;
  std::deque<Operation> operations;
  std::uint64_t revision{1};
  bool enabled{true};
  bool fmc_pending{}; // Retained native operation also blocks writes after Main reconnect.
  bool ready() const {
    return enabled && channels[0].ready() && channels[1].ready();
  }
  void connected(unsigned);
  void lost(unsigned, std::string_view);
  void record(unsigned, std::string_view, Tick);
  void control(bool);
  // Native recalls can finish without another ECON change on Main. One
  // observer read refreshes both bits; a missing reply never restores old state.
  void refresh_pause(Tick, const std::function<bool(std::string_view)> &);
  void native_pause_finished(const Operation &);
  bool pause_available() const { return ready() && !pause_refresh_requested_ && !pause_refresh_waiting_; }
  std::uint64_t submit(std::string, bool, std::uint64_t, std::string);
  std::uint64_t reposition(Operation::Move, std::uint64_t, std::string);
  std::uint64_t environment(psx::instructor::EnvironmentRequest, std::string request, std::string fingerprint);
  std::uint64_t command(std::string kind, Object, std::uint64_t revision, std::string request, std::string fingerprint);
  std::uint64_t motor_command(std::string,Object,std::string request,std::string fingerprint);
  void pump_motors(Tick,const std::function<void(Operation&,Tick,bool)>&);
  bool idle() const { return !fmc_pending && !pause_refresh_requested_ && !pause_refresh_waiting_ && !active_ && queue_.empty() && motor_queue_.empty() && std::all_of(motor_active_.begin(),motor_active_.end(),[](auto id){return id==0;}); }
  Operation *find(std::uint64_t);
  const Operation *find(std::uint64_t) const;
  // false means uncertain transport failure; the provider may already have sent
  // bytes.
  void pump(Tick, const std::function<bool(std::string_view)> &, psx::instructor::Environment * = nullptr, std::shared_ptr<const psx::instructor::WeatherStations> = {}, const std::function<void(Operation &, Tick, bool)> &command = {}, const std::function<void(psx::instructor::EnvironmentRequest &)> &prepare_environment = {}, const std::function<void(Operation &)> &finish_environment = {}, const std::function<bool(Operation &, Tick)> &preflight = {});
  std::string status(Tick) const;
  static std::string operation(const Operation &);
  std::size_t pending() const { return queue_.size()+motor_queue_.size(); }
};
struct Profile {
  std::string tab{"status"};
  bool collapsed{true};
  double ias{};
  std::vector<std::string> favorites, recent;
  std::string theme{"neutral"};
  std::string weather_units{"metric"};
};
bool valid_tab(std::string_view);
bool valid_theme(std::string_view);
bool valid_weather_units(std::string_view);
std::string normalize_ip(std::string_view);
struct Config {
  std::string psx{"127.0.0.1"}, adapter;
  std::uint16_t main_port{10747}, web_port{8090};
  bool confirmation{true};
  std::string database;
};
bool operator==(const Config &, const Config &);
} // namespace ios
