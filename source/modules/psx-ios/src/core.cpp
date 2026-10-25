#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ios/core.hpp>
#include <limits>
#include <iomanip>
#include <sstream>
#include <psx/network/io.hpp>
namespace ios {
Tick now() { return psx::instructor::now(); }
std::string quote(std::string_view s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += char(c);
    } else if (c < 32) {
      constexpr char h[] = "0123456789abcdef";
      o += "\\u00";
      o += h[c >> 4];
      o += h[c & 15];
    } else
      o += char(c);
  }
  return o + '"';
}
namespace {
struct Parser {
  std::string_view s;
  std::size_t p{};
  [[noreturn]] void bad() { throw std::invalid_argument("Invalid typed JSON"); }
  void ws() {
    while (p < s.size() &&
           (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
      ++p;
  }
  bool take(char c) {
    ws();
    if (p < s.size() && s[p] == c) {
      ++p;
      return true;
    }
    return false;
  }
  std::string str() {
    if (!take('"'))
      bad();
    std::string o;
    while (p < s.size()) {
      auto c = s[p++];
      if (c == '"')
        return o;
      if (static_cast<unsigned char>(c) < 32)
        bad();
      if (c == '\\') {
        if (p == s.size())
          bad();
        c = s[p++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        default:
          bad();
        }
      }
      if (static_cast<unsigned char>(c) >= 128)
        bad();
      o += c;
      if (o.size() > 256)
        bad();
    }
    bad();
  }
  Scalar scalar() {
    ws();
    if (p == s.size())
      bad();
    if (s[p] == '"')
      return str();
    for (auto b : {true, false}) {
      std::string_view t = b ? "true" : "false";
      if (s.substr(p, t.size()) == t) {
        p += t.size();
        return b;
      }
    }
    auto start = p;
    bool real = false;
    if (s[p] == '-') { real = true; ++p; }
    auto digit_start = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
      ++p;
    if (digit_start == p || (p - digit_start > 1 && s[digit_start] == '0'))
      bad();
    if (p < s.size() && s[p] == '.') {
      real = true; auto first = ++p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
      if (p == first) bad();
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      real = true; ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
      auto first = p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
      if (p == first) bad();
    }
    if (real) {
      double n{};
      auto r = std::from_chars(s.data() + start, s.data() + p, n);
      if (r.ec != std::errc{} || r.ptr != s.data() + p || !std::isfinite(n)) bad();
      return n;
    }
    std::uint64_t n{};
    auto r = std::from_chars(s.data() + start, s.data() + p, n);
    if (r.ec != std::errc{})
      bad();
    return n;
  }
  Value val() {
    ws();
    if(p<s.size()&&s[p]=='[') {
      ++p;Array items;
      if(!take(']'))for(;;) {
        if(!take('{'))bad();ArrayItem item;
        if(!take('}'))for(;;) {
          auto k=str();if(!take(':'))bad();
          if(!item.emplace(std::move(k),scalar()).second||item.size()>16)bad();
          if(take('}'))break;if(!take(','))bad();
        }
        items.push_back(std::move(item));if(items.size()>32)bad();
        if(take(']'))break;if(!take(','))bad();
      }
      return items;
    }
    return std::visit([](const auto &v)->Value{return v;},scalar());
  }
};
bool integer(std::string_view s, int &v) {
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return !s.empty() && r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
} // namespace
Object json(std::string_view s) {
  if (s.size() > 4096)
    throw std::invalid_argument("Body too large");
  Parser p{s};
  Object o;
  if (!p.take('{'))
    p.bad();
  if (!p.take('}'))
    for (;;) {
      auto k = p.str();
      if (!p.take(':'))
        p.bad();
      if (!o.emplace(k, p.val()).second)
        p.bad();
      if (o.size() > 32)
        p.bad();
      if (p.take('}'))
        break;
      if (!p.take(','))
        p.bad();
    }
  p.ws();
  if (p.p != s.size())
    p.bad();
  return o;
}
Object object(const ArrayItem &item) {
  Object result;for(const auto &[key,value]:item)result.emplace(key,std::visit([](const auto &v)->Value{return v;},value));return result;
}
std::string json_text(const Object &o) {
  auto scalar=[](const auto &v)->std::string {
    using T=std::decay_t<decltype(v)>;
    if constexpr(std::is_same_v<T,std::string>)return quote(v);
    else if constexpr(std::is_same_v<T,bool>)return v?"true":"false";
    else if constexpr(std::is_same_v<T,double>) {std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(17)<<v;return s.str();}
    else return std::to_string(v);
  };
  std::string out="{";bool comma=false;
  for(const auto &[key,value]:o) {
    if(comma)out+=',';comma=true;out+=quote(key)+':';
    out+=std::visit([&](const auto &v)->std::string {
      using T=std::decay_t<decltype(v)>;
      if constexpr(std::is_same_v<T,Array>) {std::string s="[";bool sep=false;for(const auto &item:v){if(sep)s+=',';sep=true;s+=json_text(object(item));}return s+"]";}
      else return scalar(v);
    },value);
  }
  return out+'}';
}
double numeric(const Object &o, const char *k) {
  auto i = o.find(k);
  if (i == o.end()) throw std::invalid_argument(std::string("Missing numeric field: ") + k);
  if (auto v = std::get_if<double>(&i->second)) return *v;
  if (auto v = std::get_if<std::uint64_t>(&i->second)) return double(*v);
  throw std::invalid_argument(std::string("Invalid numeric field: ") + k);
}
void keys(const Object &o, std::initializer_list<const char *> ks) {
  if (o.size() != ks.size())
    throw std::invalid_argument("Unexpected JSON fields");
  for (auto k : ks)
    if (!o.contains(k))
      throw std::invalid_argument("Missing JSON field");
}
Operation *Engine::find(std::uint64_t id) {
  for (auto &o : operations)
    if (o.id == id)
      return &o;
  return nullptr;
}
const Operation *Engine::find(std::uint64_t id) const {
  for (auto &o : operations)
    if (o.id == id)
      return &o;
  return nullptr;
}
void Engine::invalidate(std::string_view why) {
  pause_refresh_requested_ = pause_refresh_waiting_ = false;
  ++revision;
  for (auto id : queue_)
    if (auto o = find(id)) {
      o->state = "failed";
      o->detail = why;
    }
  queue_.clear();
  if (active_) {
    if (auto o = find(*active_)) {
      o->state = o->move&&(o->state=="checking"||o->state=="awaiting-confirmation") ? "aborted" : "indeterminate";
      o->detail = why;
    }
    active_.reset();
  }
}
void Engine::connected(unsigned c) {
  invalidate("Connection generation changed");
  auto generation = channels[c].generation + 1;
  channels[c] = {};
  channels[c].generation = generation;
  channels[c].connected = true;
}
void Engine::lost(unsigned c, std::string_view why) {
  if (channels[c].connected || channels[c].valid)
    invalidate(why);
  channels[c].connected = false;
  channels[c].valid = false;
  channels[c].engine_model.reset();
  channels[c].error = why;
}
void Engine::record(unsigned n, std::string_view s, Tick t) {
  auto &c = channels[n];
  if (!c.connected)
    return;
  if (n == 0 && active_ && find(*active_)->move && find(*active_)->state!="checking" && find(*active_)->state!="awaiting-confirmation") relocation_.record(s);
  if (s.starts_with("id=")) {
    int id{};
    c.identified = integer(s.substr(3), id) && id >= 0;
    if (!c.identified)
      lost(n, "Invalid server identity");
  } else if (s.starts_with("version=")) {
    c.version = s.substr(8);
    c.versioned = !c.version.empty() && c.version.size() <= 64;
    if (!c.versioned)
      lost(n, "Invalid server version announcement");
  } else if (s == "exit")
    lost(n, "Server exit");
  else if (s == "load1") {
    if (!c.identified || !c.versioned) {
      lost(n, "Load before identity/version");
      return;
    }
    invalidate("Native load started");
    c.stage = 1;
    c.valid = false;
    c.engine_model.reset();
  } else if (s == "load2") {
    if (c.stage != 1)
      lost(n, "Unexpected load2");
    else
      c.stage = 2;
  } else if (s == "load3") {
    if (c.stage != 2)
      lost(n, "Unexpected load3");
    else {
      c.stage = 3;
      ++revision;
    }
  } else if (n == 1 && s.starts_with("Qi1=")) {
    int model{};
    if (integer(s.substr(4), model) && model >= 0 && model <= 2)
      c.engine_model = model;
    else
      c.engine_model.reset();
  } else if (s.starts_with("Qi129=")) {
    int mask{};
    if (!integer(s.substr(6), mask) || mask < 0 || mask > 3) {
      c.valid = false;
      invalidate("Invalid pause mask");
      return;
    }
    if (n == 1 && (!c.valid || mask != c.mask))
      ++revision;
    c.mask = mask;
    c.valid = true;
    c.observed = t;
    if (n == 1) pause_refresh_waiting_ = false;
    if (n == 1 && active_ && !find(*active_)->move && !find(*active_)->environment && !find(*active_)->command) {
      auto o = find(*active_);
      if (o) {
        o->state = mask == o->target ? "observed" : "superseded";
        o->detail =
            mask == o->target
                ? "Register observation only; effective pause unverified"
                : "Conflicting observer value; no retry";
      }
      active_.reset();
    }
  }
}
void Engine::control(bool value) {
  if (enabled != value) {
    invalidate(value ? "Connect requested" : "Stopped by instructor");
    enabled = value;
  }
}
void Engine::native_pause_finished(const Operation &op) {
  if ((op.kind != "fmc-recall" && op.kind != "situation-load") || !op.terminal() || op.id <= pause_refresh_operation_) return;
  pause_refresh_operation_ = op.id;
  pause_refresh_requested_ = true;
  ++revision;
}
void Engine::refresh_pause(Tick t, const std::function<bool(std::string_view)> &read) {
  if (!ready()) return;
  if (pause_refresh_requested_) {
    pause_refresh_requested_ = false;
    pause_refresh_waiting_ = true;
    pause_refresh_deadline_ = t + 5000;
    if (!read("bang\n")) lost(1, "Pause state read unavailable");
  } else if (pause_refresh_waiting_ && t >= pause_refresh_deadline_) {
    lost(1, "Pause state read timed out");
  }
}
std::uint64_t Engine::submit(std::string kind, bool desired, std::uint64_t rev,
                             std::string request) {
  if (kind != "situation-save" && kind != "situation-load" && kind != "situation-delete" && kind != "pause" && kind != "motion-freeze" && kind != "reposition" && kind != "weather" && kind != "time" && kind != "nearest" && kind != "manual-weather" && kind != "failure-set" && kind != "failure-clear" && kind != "failure-reset-all" && kind != "tcas" && kind != "runway-candidates" && kind != "runway-place" && kind != "runway-remove" && kind != "failure-batch" && kind != "weather-events" && kind != "crash-reset" && kind != "weight-set" && kind != "weight-action" && kind != "fmc-save" && kind != "fmc-recall")
    throw std::invalid_argument("Unsupported operation");
  if (request.empty() || request.size() > 64 ||
      !std::all_of(request.begin(), request.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
      }))
    throw std::invalid_argument("Invalid request ID");
  for (auto &o : operations) {
    if(o.terrain.request==request)throw std::runtime_error("Request ID conflict");
    if (o.request == request) {
      if (o.kind != kind || o.desired != desired || o.revision != rev)
        throw std::runtime_error("Request ID conflict");
      return o.id;
    }
  }
  if ((kind != "runway-remove" && !ready()) || revision != rev)
    throw std::runtime_error("State precondition changed or PSX unavailable");
  if (queue_.size()+motor_queue_.size() >= 128)
    throw std::runtime_error("Command queue full");
  if (operations.size() == 256) {
    auto i = std::find_if(operations.begin(), operations.end(),
                          [](const Operation &o) { return o.terminal(); });
    if (i == operations.end())
      throw std::runtime_error("Operation history full");
    operations.erase(i);
  }
  Operation o;
  o.id = next_++;
  o.request = std::move(request);
  o.kind = std::move(kind);
  o.desired = desired;
  o.revision = rev;
  queue_.push_back(o.id);
  operations.push_back(std::move(o));
  return operations.back().id;
}
std::uint64_t Engine::reposition(Operation::Move move, std::uint64_t rev, std::string request) {
  for (const auto &o : operations)
    if (o.request == request) {
      if (!o.move || o.move->preview != move.preview || o.revision != rev)
        throw std::runtime_error("Request ID conflict");
      return o.id;
    }
  auto id = submit("reposition", false, rev, std::move(request));
  find(id)->move = std::move(move);
  return id;
}
std::uint64_t Engine::environment(psx::instructor::EnvironmentRequest request, std::string id, std::string fingerprint) {
  for(const auto &op:operations) if(op.request==id) {
    if(!op.environment || op.fingerprint!=fingerprint) throw std::runtime_error("Request ID conflict"); return op.id;
  }
  using K=psx::instructor::EnvironmentRequest::Kind;
  auto kind=request.kind==K::time ? "time" : request.kind==K::nearest ? "nearest" : request.kind==K::manual ? "manual-weather" : request.kind==K::events ? "weather-events" : "weather";
  auto n=submit(kind,false,request.expected_revision.value_or(revision),std::move(id)); find(n)->environment=std::move(request); find(n)->fingerprint=std::move(fingerprint); return n;
}
std::uint64_t Engine::command(std::string kind, Object command, std::uint64_t rev, std::string request, std::string fingerprint) {
  for (const auto &op : operations) if (op.request == request) {
    if (!op.command || op.kind != kind || op.revision != rev || op.fingerprint != fingerprint) throw std::runtime_error("Request ID conflict");
    return op.id;
  }
  auto id = submit(std::move(kind), false, rev, std::move(request));
  auto op = find(id); op->command = std::move(command); op->fingerprint = std::move(fingerprint); return id;
}
std::uint64_t Engine::motor_command(std::string kind,Object command,std::string request,std::string fingerprint){
  if(kind!="motor-reset-home" && kind!="motor-stop" && kind!="actuator-reset-home" && kind!="actuator-stop")throw std::invalid_argument("Invalid local motor operation");
  if(request.empty() || request.size()>64 || !std::all_of(request.begin(),request.end(),[](unsigned char c){return std::isalnum(c)||c=='-'||c=='_';}))throw std::invalid_argument("Invalid request ID");
  for(const auto& op:operations){if(op.terrain.request==request)throw std::runtime_error("Request ID conflict");if(op.request==request){if(op.kind!=kind || op.fingerprint!=fingerprint)throw std::runtime_error("Request ID conflict");return op.id;}}
  if(field<std::uint64_t>(command,"motor")>=(kind.starts_with("actuator-")?60u:motor_slots))throw std::invalid_argument("Invalid motor index");
  if(motor_queue_.size()>=32 || queue_.size()+motor_queue_.size()>=128)throw std::runtime_error("Command queue full");
  if(operations.size()>=256){const auto i=std::find_if(operations.begin(),operations.end(),[](const auto& o){return o.terminal();});if(i==operations.end())throw std::runtime_error("Operation history full");operations.erase(i);}
  Operation op;op.id=next_++;op.kind=std::move(kind);op.command=std::move(command);op.request=std::move(request);op.fingerprint=std::move(fingerprint);motor_queue_.push_back(op.id);operations.push_back(std::move(op));return operations.back().id;
}
void Engine::pump_motors(Tick t,const std::function<void(Operation&,Tick,bool)>& dispatch){
  auto stop=std::find_if(motor_queue_.begin(),motor_queue_.end(),[&](auto id){const auto* op=find(id);return op && op->kind.ends_with("-stop");});
  if(stop!=motor_queue_.end()){
    const auto id=*stop;motor_queue_.erase(stop);auto* op=find(id);const auto selected=unsigned(field<std::uint64_t>(*op->command,"motor"));
    if(motor_active_[selected]){auto* active=find(motor_active_[selected]);if(active){active->state="indeterminate";active->detail="Stop requested; physical cancellation awaits HWIO confirmation";}motor_active_[selected]=0;}
    for(auto i=motor_queue_.begin();i!=motor_queue_.end();){auto* queued=find(*i);if(queued && queued->kind.ends_with("reset-home") && field<std::uint64_t>(*queued->command,"motor")==selected){queued->state="superseded";queued->detail="Stopped before motor dispatch";i=motor_queue_.erase(i);}else ++i;}
    op->state="sent";op->sent=t;motor_active_[selected]=id;dispatch(*op,t,true);if(op->terminal())motor_active_[selected]=0;
  }
  for(auto& id:motor_active_)if(id){auto* op=find(id);if(op){dispatch(*op,t,false);if(op->terminal())id=0;}else id=0;}
  if(motor_queue_.empty())return;
  auto queued=std::find_if(motor_queue_.begin(),motor_queue_.end(),[&](auto id){const auto* op=find(id);return op && !motor_active_[unsigned(field<std::uint64_t>(*op->command,"motor"))];});
  if(queued==motor_queue_.end())return;const auto id=*queued;motor_queue_.erase(queued);auto* op=find(id);if(!op)return;
  const auto selected=unsigned(field<std::uint64_t>(*op->command,"motor"));op->state="sent";op->sent=t;motor_active_[selected]=id;dispatch(*op,t,true);if(op->terminal())motor_active_[selected]=0;
}
void Engine::pump(Tick t, const std::function<bool(std::string_view)> &send, psx::instructor::Environment *environment, std::shared_ptr<const psx::instructor::WeatherStations> stations, const std::function<void(Operation &, Tick, bool)> &command, const std::function<void(psx::instructor::EnvironmentRequest &)> &prepare_environment, const std::function<void(Operation &)> &finish_environment, const std::function<bool(Operation &, Tick)> &preflight) {
  if (active_) {
    auto o = find(*active_);
    if(o&&o->terminal()){active_.reset();return;}
    if (o && o->command) {
      if (command) command(*o, t, false); else { o->state = "failed"; o->detail = "Non-normal sender unavailable"; }
      if (o->terminal()) {
        native_pause_finished(*o);
        active_.reset();
      }
      return;
    }
    if(o && o->environment && environment) {
      environment->pump(t); o->detail=environment->detail();
      if(!environment->busy()) { o->state=environment->state(); if(finish_environment)finish_environment(*o);active_.reset(); }
      return;
    }
    if (o && o->move) {
      if(o->state=="checking"||o->state=="awaiting-confirmation") {
        if(!ready()||revision!=o->revision){o->state="superseded";o->detail="PSX changed before repositioning";active_.reset();return;}
        if(preflight&&!preflight(*o,t)){if(o->terminal())active_.reset();return;}
        o->state="sent";o->sent=t;relocation_.start(*o->move,t,send);
        o->state=relocation_.state();o->detail=relocation_.detail();
        if(relocation_.finished())active_.reset();return;
      }
      relocation_.pump(t, send);
      o->target = relocation_.finish_mask();
      o->state = relocation_.state();
      o->detail = relocation_.detail();
      if (relocation_.finished()) active_.reset();
      return;
    }
    if (o && t - o->sent >= 2000) {
      o->state = "indeterminate";
      o->detail = "Observer deadline elapsed; no replay";
      active_.reset();
    }
    return;
  }
  if (queue_.empty() || fmc_pending || pause_refresh_requested_ || pause_refresh_waiting_)
    return;
  auto id = queue_.front();
  queue_.pop_front();
  auto o = find(id);
  if (!o)
    return;
  if ((!ready() && o->kind != "runway-remove") || ((!o->environment || o->environment->expected_revision) && revision != o->revision)) {
    o->state = "superseded";
    o->detail = "State changed before dispatch";
    return;
  }
  if(o->environment) {
    if(!environment) { o->state="failed"; o->detail="Environment sender unavailable"; return; }
    try { if(prepare_environment)prepare_environment(*o->environment);environment->start(*o->environment,channels[1].mask,std::move(stations)); o->state="sent"; o->sent=t; active_=id; }
    catch(const std::exception &e) { o->state="failed"; o->detail=e.what(); }
    return;
  }
  if (o->command) {
    o->state = "sent"; o->sent = t; active_ = id;
    if (command) command(*o, t, true); else { o->state = "failed"; o->detail = "Non-normal sender unavailable"; }
    if (o->terminal()) { native_pause_finished(*o); active_.reset(); }
    return;
  }
  if (o->move) {
    o->state = "checking";
    active_ = id;
    if(preflight&&!preflight(*o,t)){if(o->terminal())active_.reset();return;}
    o->state="sent";o->sent=t;
    relocation_.start(*o->move, t, send);
    o->state = relocation_.state();
    o->detail = relocation_.detail();
    if (relocation_.finished()) active_.reset();
    return;
  }
  auto mask = channels[1].mask;
  int bit = o->kind == "pause" ? 1 : 2;
  o->target = o->desired ? (mask | bit) : (mask & ~bit);
  if (mask == o->target) {
    o->state = "observed";
    o->detail = "Observed no-op; effective pause unverified";
    return;
  }
  o->state = "sent";
  o->sent = t;
  active_ = id;
  if (!send("Qi129=" + std::to_string(o->target) + "\n")) {
    o->state = "indeterminate";
    o->detail = "Transport failure after dispatch; bytes may have been sent";
    active_.reset();
  }
}
std::string Engine::operation(const Operation &o) {
  return "{\"id\":" + std::to_string(o.id) +
         ",\"request_id\":" + quote(o.request) + ",\"kind\":" + quote(o.kind) +
         ",\"desired\":" + (o.desired ? "true" : "false") +
         ",\"state\":" + quote(o.state) + ",\"target_mask\":" +
         (o.target < 0 ? "null" : std::to_string(o.target)) +
         ",\"detail\":" + quote(o.detail) +
         ",\"terrain\":" + (o.move ? "{\"target_id\":"+quote(o.move->target_id)+",\"verdict\":"+quote(o.terrain.verdict)+",\"decision\":"+quote(o.terrain.decision)+"}" : "null") +
         ",\"result\":" + o.result_json +
         ",\"effective_pause_verified\":false}";
}
std::string Engine::status(Tick t) const {
  constexpr std::array<std::string_view, 3> engines{
      "CF6", "PW4000", "RB211"};
  const auto model = ready() ? channels[1].engine_model : std::nullopt;
  const auto engine = model ? engines[*model] : std::string_view{};
  const auto identity = "Boeing 747-400" +
      (model ? " (" + std::string(engine) + ")" : std::string{});
  std::string s =
      "{\"ready\":" + std::string(ready() ? "true" : "false") +
      ",\"command_busy\":" + (idle()?"false":"true") +
      ",\"enabled\":" + (enabled ? "true" : "false") +
      ",\"revision\":" + std::to_string(revision) + ",\"observed_mask\":" +
      (pause_available() ? std::to_string(channels[1].mask) : "null") +
      ",\"aircraft\":{\"identity\":" + quote(identity) +
      ",\"engine_type\":" + (model ? quote(engine) : "null") + "}" +
      ",\"effective_pause_verified\":false,\"channels\":[";
  for (unsigned n = 0; n < 2; ++n) {
    const auto &c = channels[n];
    if (n)
      s += ',';
    s +=
        "{\"role\":" + quote(n ? "observer" : "writer") +
        ",\"connected\":" + (c.connected ? "true" : "false") +
        ",\"synchronized\":" + (c.ready() ? "true" : "false") +
        ",\"generation\":" + std::to_string(c.generation) +
        ",\"load_stage\":" + std::to_string(c.stage) +
        ",\"version\":" + quote(c.version) + ",\"version_qualified\":" +
        (c.version == "10.195" || c.version == "10.195 NG" ? "true" : "false") +
        ",\"error\":" + quote(c.error) + ",\"observation_age_ms\":" +
        (c.observed ? std::to_string(t >= c.observed ? t - c.observed : 0)
                    : "null") +
        "}";
  }
  const auto pending=std::find_if(operations.begin(),operations.end(),[](const auto &o){return o.move&&(o.state=="checking"||o.state=="awaiting-confirmation");});
  return s + "],\"reposition_preflight\":" + (pending==operations.end()?"null":operation(*pending)) + ",\"operation\":" +
         (operations.empty() ? "null" : operation(operations.back())) + "}";
}
bool valid_tab(std::string_view t) {
  return t == "position" || t == "time" || t == "weather" || t == "situations" || t == "non-normal" || t == "weight-service" || t == "status";
}
bool valid_theme(std::string_view t) {
  return t == "neutral" || t == "teal-orange" || t == "warm-graphite";
}
bool valid_weather_units(std::string_view t) {
  return t == "metric" || t == "imperial";
}
std::string normalize_ip(std::string_view s) {
  auto e = psx::network::Endpoint::parse(std::string(s), 0);
  auto h = e.host();
  if (h.starts_with("::ffff:")) {
    auto v4 = h.substr(7);
    auto mapped = psx::network::Endpoint::parse(v4, 0);
    if (mapped.address.ss_family == AF_INET)
      return mapped.host();
  }
  return h;
}
bool operator==(const Config &a, const Config &b) {
  return a.psx == b.psx && a.adapter == b.adapter &&
         a.main_port == b.main_port && a.web_port == b.web_port &&
         a.confirmation == b.confirmation && a.database == b.database;
}
} // namespace ios
