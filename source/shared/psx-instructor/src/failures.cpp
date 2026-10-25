#include <psx/instructor/failures.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <sstream>
#include <stdexcept>
namespace psx::instructor {
namespace {
constexpr std::array<FailureInfo, failure_count> catalogue{{
#include "failure_catalogue.inc"
}};
constexpr std::array<std::string_view, 7> criteria{{"ias", "altitude", "heading", "distance", "utc_time", "random", "immediate"}};
auto find_info(unsigned id) { return std::lower_bound(catalogue.begin(), catalogue.end(), id, [](const auto &info, auto n) {return info.id < n;}); }
std::size_t index(unsigned id) { auto i = find_info(id); if (i == catalogue.end() || i->id != id) throw std::invalid_argument("Unknown failure ID"); return std::size_t(i - catalogue.begin()); }
int number(std::string_view s) {
  int n{}; auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) throw std::invalid_argument("Invalid failure value");
  return n;
}
void validate(FailureCriterion criterion, std::string_view value, bool allow_off) {
  if (allow_off && value == "-99") return;
  auto mode = static_cast<unsigned>(criterion);
  if (mode >= criteria.size()) throw std::invalid_argument("Invalid failure criterion");
  if (mode == 4 || mode == 6) {
    if (value.size() != 6 || !std::all_of(value.begin(), value.end(), [](char c) {return c >= '0' && c <= '9';}) ||
        number(value.substr(0, 2)) > 23 || number(value.substr(2, 2)) > 59 || number(value.substr(4, 2)) > 59)
      throw std::invalid_argument("Failure time must be HHMMSS UTC");
    return;
  }
  constexpr int low[]{0, -90, 0, 0, 0, 1}, high[]{600, 50000, 359, 9999, 0, 9999};
  auto n = number(value); if (n < low[mode] || n > high[mode]) throw std::invalid_argument("Failure value outside native limits");
}
std::string lower(std::string_view text) {
  std::string s(text); for (auto &c : s) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A')); return s;
}
bool equal(const FailureSetting &a, const FailureSetting &b) {
  return a.severe == b.severe && a.criterion == b.criterion && number(a.value) == number(b.value);
}
}
std::span<const FailureInfo> failure_catalogue() { return catalogue; }
const FailureInfo &failure_info(unsigned id) {
  return catalogue[index(id)];
}
std::string_view criterion_name(FailureCriterion c) {
  if (static_cast<unsigned>(c) >= criteria.size()) throw std::invalid_argument("Invalid failure criterion"); return criteria[static_cast<unsigned>(c)];
}
FailureCriterion failure_criterion(std::string_view text) {
  auto i = std::find(criteria.begin(), criteria.end(), text); if (i == criteria.end()) throw std::invalid_argument("Unknown failure criterion");
  return static_cast<FailureCriterion>(i - criteria.begin());
}
std::vector<unsigned> search_failures(std::string_view text) {
  if (text.size() > 256) throw std::invalid_argument("Failure search too long");
  std::istringstream words(lower(text)); std::vector<std::string> terms; std::string term;
  while (words >> term) terms.push_back(term);
  std::vector<unsigned> result;
  for (auto &info : catalogue) {
    auto haystack = lower(std::string(info.name) + ' ' + std::string(info.category) + ' ' + std::string(info.severe) + ' ' + std::string(info.non_severe) + ' ' + std::to_string(info.id));
    if(haystack.find("eng ")!=haystack.npos)haystack+=" engine";
    if (std::all_of(terms.begin(), terms.end(), [&](const auto &s) {return haystack.find(s) != std::string::npos;})) result.push_back(info.id);
  }
  return result;
}
void validate_failure_edit(const FailureEdit &edit) {
  failure_info(edit.id); criterion_name(edit.criterion);
  if (edit.delay_minutes) {
    if (edit.criterion != FailureCriterion::utc_time || !edit.value.empty() || *edit.delay_minutes < 1 || *edit.delay_minutes > 1439)
      throw std::invalid_argument("Simulated delay must be 1 to 1439 minutes");
    return;
  }
  if (edit.criterion == FailureCriterion::immediate) { if (!edit.value.empty()) throw std::invalid_argument("Immediate activation uses the PSX UTC clock"); }
  else validate(edit.criterion, edit.value, false);
}
FailureSetting FailureSetting::parse(std::string_view text) {
  auto a = text.find(';'), b = a == text.npos ? text.npos : text.find(';', a + 1);
  if (a != 1 || b != 3 || (text[0] != '0' && text[0] != '1') || text[2] < '0' || text[2] > '6' || text.find(';', b + 1) != text.npos)
    throw std::invalid_argument("Malformed failure record");
  FailureSetting s{text[0] == '1', static_cast<FailureCriterion>(text[2] - '0'), std::string(text.substr(b + 1))};
  validate(s.criterion, s.value, true); return s;
}
std::string FailureSetting::encode() const { return std::string(severe ? "1;" : "0;") + std::to_string(static_cast<unsigned>(criterion)) + ';' + value; }
const std::optional<FailureSetting> &Failures::setting(unsigned id) const { return settings_[index(id)]; }
void Failures::set(const FailureEdit &edit, Tick t) {
  set_batch(std::span(&edit, 1), t);
}
void Failures::clear(unsigned id, Tick t) {
  clear_batch(std::span(&id, 1), t);
}
std::string Failures::delayed_utc(unsigned minutes) const {
  if (!utc_) throw std::invalid_argument("PSX UTC clock unavailable");
  unsigned seconds = unsigned(number(std::string_view(*utc_).substr(0, 2))) * 3600 +
    unsigned(number(std::string_view(*utc_).substr(2, 2))) * 60 + unsigned(number(std::string_view(*utc_).substr(4, 2)));
  seconds = (seconds + minutes * 60) % 86400;
  char value[7]{};
  std::snprintf(value, sizeof(value), "%02u%02u%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  return value;
}
void Failures::begin(std::vector<FailureItemResult> items, Tick t) {
  if (busy()) throw std::logic_error("Failure sender busy");
  if (items.empty() || items.size() > 32) throw std::invalid_argument("Select 1 to 32 failures");
  std::string packet, expected;
  std::vector<unsigned> ids;
  for (const auto &item : items) {
    if (std::find(ids.begin(), ids.end(), item.id) != ids.end()) throw std::invalid_argument("Duplicate failure in batch");
    ids.push_back(item.id);
    auto encoded = item.expected.encode();
    packet += "Qs" + std::to_string(item.id) + '=' + encoded + '\n';
    if (!expected.empty()) expected += ',';
    expected += items.size() == 1 ? encoded : std::to_string(item.id) + '=' + encoded;
  }
  batch_ = std::move(items);
  exchange_.begin(packet + "Qi130=1\n", std::move(expected), t);
}
void Failures::set_batch(std::span<const FailureEdit> edits, Tick t) {
  if (busy()) throw std::logic_error("Failure sender busy");
  if (edits.empty() || edits.size() > 32) throw std::invalid_argument("Select 1 to 32 failures");
  std::vector<FailureItemResult> items;
  for (const auto &edit : edits) {
    validate_failure_edit(edit);
    auto value = edit.value;
    if (edit.delay_minutes) value = delayed_utc(*edit.delay_minutes);
    else if (edit.criterion == FailureCriterion::immediate) {
      if (!utc_) throw std::invalid_argument("PSX UTC clock unavailable"); value = *utc_;
    }
    items.push_back({edit.id, {edit.severe, edit.criterion, std::move(value)}, SettingsStatus::pending, {}});
  }
  begin(std::move(items), t);
}
void Failures::clear_batch(std::span<const unsigned> ids, Tick t) {
  if (busy()) throw std::logic_error("Failure sender busy");
  std::vector<FailureItemResult> items;
  for (unsigned id : ids) {
    const auto &s = setting(id);
    if (!s) throw std::invalid_argument("Failure settings unavailable");
    items.push_back({id, {s->severe, FailureCriterion::immediate, "-99"}, SettingsStatus::pending, {}});
  }
  begin(std::move(items), t);
}
void Failures::finish_batch() {
  if (!exchange_.reading() || std::any_of(batch_.begin(), batch_.end(), [](const auto &i) {return i.status == SettingsStatus::pending;})) return;
  auto status = SettingsStatus::match;
  std::string returned;
  for (const auto &i : batch_) {
    if (i.status == SettingsStatus::unavailable) status = SettingsStatus::unavailable;
    else if (i.status == SettingsStatus::different && status != SettingsStatus::unavailable) status = SettingsStatus::different;
    else if (i.status == SettingsStatus::activated && status == SettingsStatus::match) status = SettingsStatus::activated;
    if (i.returned) {
      if (!returned.empty()) returned += ',';
      returned += batch_.size() == 1 ? i.returned->encode() : std::to_string(i.id) + '=' + i.returned->encode();
    }
  }
  exchange_.finish(status, status == SettingsStatus::match ? "Configured failure readback matches; physical effect unverified" :
    status == SettingsStatus::activated ? "Readback is active after arming; no re-arm" :
    status == SettingsStatus::different ? "Configured failure readback differs; no retry" : "Malformed failure readback; no retry", returned);
}
void Failures::pump(Tick t) {
  exchange_.pump(t);
  if (!exchange_.busy()) for (auto &i : batch_) if (i.status == SettingsStatus::pending) i.status = SettingsStatus::unavailable;
}
void Failures::disconnected() { settings_ = {}; utc_.reset(); exchange_.cancel(); }
void Failures::record(std::string_view line) {
  if (line == "load1" || line == "exit") { disconnected(); return; }
  if (line.starts_with("Qs123=")) {
    utc_.reset(); std::int64_t ms{}; auto text = line.substr(6); auto r = std::from_chars(text.data(), text.data() + text.size(), ms);
    if (r.ec == std::errc{} && r.ptr == text.data() + text.size()) {
      auto seconds = ((ms % 86400000 + 86400000) % 86400000) / 1000;
      char value[7]{}; std::snprintf(value, sizeof(value), "%02d%02d%02d", int(seconds / 3600), int(seconds / 60 % 60), int(seconds % 60)); utc_ = value;
    }
    return;
  }
  if (!line.starts_with("Qs")) return;
  auto equal_pos = line.find('='); if (equal_pos == line.npos) return;
  auto id = integer(line.substr(2, equal_pos - 2)); if (!id || *id < 0) return;
  auto info = find_info(unsigned(*id)); if (info == catalogue.end() || info->id != unsigned(*id)) return;
  auto &s = settings_[std::size_t(info - catalogue.begin())]; s.reset();
  auto item = std::find_if(batch_.begin(), batch_.end(), [&](const auto &i) {return i.id == unsigned(*id);});
  try { s = FailureSetting::parse(line.substr(equal_pos + 1)); }
  catch (const std::invalid_argument &) {
    if (exchange_.reading() && item != batch_.end() && item->status == SettingsStatus::pending) { item->status = SettingsStatus::unavailable; finish_batch(); } return;
  }
  if (!exchange_.reading() || item == batch_.end() || item->status != SettingsStatus::pending) return;
  item->returned = s;
  item->status = equal(item->expected, *s) ? SettingsStatus::match :
    item->expected.armed() && s->active() && item->expected.severe == s->severe ? SettingsStatus::activated : SettingsStatus::different;
  finish_batch();
}
}
