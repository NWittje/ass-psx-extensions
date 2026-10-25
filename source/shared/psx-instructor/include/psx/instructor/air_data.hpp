#pragma once

namespace psx::instructor {
// PSX ideal indication arithmetic. IAS is treated as ideal CAS; no instrument
// correction, iteration, departure ratio or ongoing speed hold is applied.
class AirData {
  double impact_{};
public:
  struct Correction { double pressure_altitude_ft{}, tas_kt{}; };
  explicit AirData(double requested_ias_kt);
  Correction correct(double altitude_msl_ft, double oat_c, double qnh_hpa) const;
  static double ideal_ias(double tas_kt, double pressure_altitude_ft, double oat_c);
};
}
