#include <psx/instructor/air_data.hpp>
#include <cmath>
#include <stdexcept>

namespace psx::instructor {
namespace {
// PSX-compatible pressure ratio: altitude is in feet, the result dimensionless.
// Preserve the inherited decimal approximations and evaluation order.
double pressure(double altitude) {
  const double base = 1.0 - 6.876e-6 * altitude;
  if (!std::isfinite(altitude) || base <= 0)
    throw std::invalid_argument("Invalid pressure altitude for IAS correction");
  return std::pow(base, 5.256);
}
// OAT is degrees Celsius; this inherited conversion returns knots.
double sound_speed(double oat) {
  if (!std::isfinite(oat) || oat <= -273.1)
    throw std::invalid_argument("Invalid OAT for IAS correction");
  return 38.97 * std::sqrt(oat + 273.2);
}
}

AirData::AirData(double ias) {
  if (!std::isfinite(ias) || ias < 0 || ias > 1500)
    throw std::invalid_argument("Invalid requested IAS");
  const double ratio = ias / 661.48;
  // Cache the dimensionless impact-pressure term once for the requested IAS.
  impact_ = std::pow(1.0 + ratio * ratio / 5.0, 1.0 / 0.28571429) - 1.0;
}

AirData::Correction AirData::correct(double altitude, double oat, double qnh) const {
  if (!std::isfinite(altitude) || !std::isfinite(qnh) || qnh <= 0)
    throw std::invalid_argument("Invalid altitude or QNH for IAS correction");
  // Destination altitude is feet MSL; configured QNH is hPa. The existing
  // 27 ft/hPa approximation gives pressure altitude for this finite correction.
  const double hp = altitude - 27.0 * (qnh - 1013.25);
  const double mach = std::sqrt(5.0 * (std::pow(1.0 + impact_ / pressure(hp), 1.0 / 3.5) - 1.0));
  const double tas = sound_speed(oat) * mach;
  if (!std::isfinite(tas) || tas < 0 || tas > 1500)
    throw std::invalid_argument("Cannot calculate TAS from received weather");
  return {hp, tas};
}

double AirData::ideal_ias(double tas, double altitude, double oat) {
  if (!std::isfinite(tas) || tas < 0 || tas > 1500)
    throw std::invalid_argument("Invalid TAS");
  const double mach = tas / sound_speed(oat);
  const double impact = std::pow(1.0 + mach * mach / 5.0, 3.5) - 1.0;
  return 661.48 * std::sqrt(5.0 * (std::pow(1.0 + pressure(altitude) * impact, 0.28571429) - 1.0));
}
}
