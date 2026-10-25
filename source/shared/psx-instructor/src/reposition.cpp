#include <psx/instructor/reposition.hpp>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace psx::instructor {
double corrected_tas(double ias, double altitude, double oat, double qnh) {
  return AirData(ias).correct(altitude, oat, qnh).tas_kt;
}
std::string placement(const RepositionTarget &m, double tas) {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  s << std::setprecision(17) << "Qi257=" << (m.ground ? 1 : 0)
    << "\nQs122=1;" << std::lround(m.pitch * 1000.0) << ";0;"
    << std::lround(m.heading * 1000.0) << ';' << std::lround(m.altitude)
    << ";0;" << std::lround(tas) << ";0;" << m.lat << ';' << m.lon << ';'
    << std::lround(m.terrain * 10.0) << '\n';
  return s.str();
}
void Reposition::report(std::string_view text) const noexcept {
  try { if (log_) log_(text); } catch (...) {} // Diagnostics do not change the sender.
}
void Reposition::begin(std::string_view name) { stage_ = name; detail_ = stage_; report(stage_ + " BEGIN"); }
void Reposition::success(std::string_view text) { detail_ = text; report(stage_ + " SUCCESS: " + detail_); }
bool Reposition::write(const Send &send, const std::string &text) {
  if (send(text)) return true;
  stop("PSX disconnected; sending stopped");
  return false;
}
std::string Reposition::missing_weather() const {
  std::string s = "; missing:";
  if (!oat_) s += " OAT(Qs483)";
  if (zone_ < 0) s += " selected zone(Qi240) selected QNH(zone unknown)";
  else if (!pressures_[zone_]) s += " QNH(Qs" + std::to_string(328 + zone_) + ")";
  return s;
}
void Reposition::stop(std::string reason, bool uncertain) {
  state_ = uncertain ? "indeterminate" : "failed";
  detail_ = std::move(reason);
  report(stage_ + " ERROR: " + detail_);
  phase_ = Phase::finished;
}
void Reposition::start(const RepositionTarget &target, Tick t, const Send &send, Log log) {
  *this = Reposition{};
  target_ = target; log_ = std::move(log); state_ = "sent";
  try { if (!target_.ground) air_data_.emplace(target_.ias); }
  catch (const std::exception &e) { stop(e.what(), false); return; }
  begin("pause query");
  phase_ = Phase::pause_query; deadline_ = t + 5000;
  write(send, "bang\n");
}
void Reposition::record(std::string_view line) {
  if (phase_ == Phase::pause_query && line.starts_with("Qi129=")) {
    auto value = integer(line.substr(6));
    if (!value || *value < 0 || *value > 3) { stop("Invalid PSX pause reply", false); return; }
    mask_ = *value;
  }
  // Ignore records processed before the weather request. Public TCP records have
  // no timestamp; accepting a numeric value does not prove destination freshness.
  if (phase_ != Phase::running && phase_ != Phase::weather) return;
  if (line.starts_with("Qs483=")) {
    auto payload = line.substr(6);
    if (!payload.empty() && payload.find_first_not_of('#') == std::string_view::npos)
      report("weather read INFO: Qs483 placeholder (no OAT yet)");
    else {
      auto value = integer(payload, 1);
      if (value && *value > -2731 && *value < 1000) {
        oat_ = *value / 10.0;
        report("weather read INFO: Qs483 OAT=" + std::to_string(*oat_) + " C");
      } else report("weather read INFO: Qs483 OAT token missing or invalid; raw=" + std::string(payload.substr(0, 300)));
    }
  } else if (line.starts_with("Qi240=")) {
    auto value = integer(line.substr(6));
    if (value && *value >= 0 && *value < 8) {
      zone_ = *value;
      report("weather read INFO: Qi240 zone=" + std::to_string(zone_));
    }
  } else if (line.size() >= 6 && line.starts_with("Qs") && line[5] == '=') {
    auto code = integer(line.substr(2, 3));
    if (code && *code >= 328 && *code <= 335) {
      auto value = integer(line.substr(6), 23);
      if (value && *value >= 2700 && *value <= 3200) {
        pressures_[*code - 328] = std::round(*value * 0.33863887);
        report("weather read INFO: Qs" + std::to_string(*code) + " QNH=" + std::to_string(*pressures_[*code - 328]) + " hPa");
      }
    }
  }
}
void Reposition::pump(Tick t, const Send &send) {
  if (phase_ == Phase::pause_query && mask_ >= 0) {
    success("received Qi129=" + std::to_string(mask_));
    finish_mask_ = 1 | (mask_ & 2);
    begin("initial pause");
    if (!(mask_ & 1) && !write(send, "Qi129=" + std::to_string(finish_mask_) + "\n")) return;
    success(mask_ & 1 ? "already requested/observed paused" : "sent Qi129=" + std::to_string(finish_mask_));
    phase_ = Phase::placement;
  }
  if (phase_ == Phase::placement) {
    begin("first placement");
    if (!write(send, placement(target_, target_.ground ? 0.0 : target_.ias))) return;
    success("sent Qi257 and Qs122; provisional speed=" + std::to_string(target_.ground ? 0.0 : target_.ias));
    begin("weather request");
    if (!write(send, "Qi129=2\n")) return;
    report("weather request INFO: sent Qi129=2");
    phase_ = Phase::running;
    if (!write(send, "demand=Qs483\nbang\n")) return;
    success("sent demand=Qs483 and bang while unpaused with Motion Freeze");
    begin("one-second frozen interval");
    deadline_ = t + 1000;
  } else if (phase_ == Phase::running && t >= deadline_) {
    success("one-second sender interval elapsed");
    begin("final pause");
    if (!write(send, "Qi129=" + std::to_string(finish_mask_) + "\n")) return;
    success("sent Qi129=" + std::to_string(finish_mask_));
    begin("weather read");
    report("weather read INFO: reading replies requested during frozen interval; no further weather request");
    phase_ = Phase::weather; deadline_ = t + 5000;
  }
  if (phase_ == Phase::weather && oat_ && zone_ >= 0 && pressures_[zone_]) {
    success("OAT=" + std::to_string(*oat_) + " C; selected QNH=" + std::to_string(*pressures_[zone_]) + " hPa");
    double tas = 0.0;
    double hp = target_.altitude - 27.0 * (*pressures_[zone_] - 1013.25);
    try {
      if (air_data_) { auto correction = air_data_->correct(target_.altitude, *oat_, *pressures_[zone_]);
        tas = correction.tas_kt; hp = correction.pressure_altitude_ft; }
    } catch (const std::exception &e) { stop(e.what(), false); return; }
    report("air data INFO: requested IAS=" + std::to_string(target_.ias) + " kt; OAT=" +
      std::to_string(*oat_) + " C; selected QNH=" + std::to_string(*pressures_[zone_]) +
      " hPa; pressure altitude=" + std::to_string(hp) + " ft; calculated TAS=" +
      std::to_string(tas) + " kt; encoded TAS=" + std::to_string(std::lround(tas)) + " kt");
    begin("second placement");
    if (!write(send, placement(target_, tas))) return;
    success("sent Qi257 and Qs122; corrective speed=" + std::to_string(tas));
    state_ = "completed";
    detail_ = "Sender finished: corrective placement sent, full pause requested. PSX application is not verified.";
    phase_ = Phase::finished;
  } else if ((phase_ == Phase::pause_query || phase_ == Phase::weather) && t >= deadline_) {
    stop("PSX did not reply within five seconds" + (phase_ == Phase::weather ? missing_weather() : ""), false);
  }
}
}
