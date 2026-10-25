#pragma once
#include "airport_database.hpp"
#include <chrono>
#include <memory>

namespace psx::instructor {
struct CivilDate { int year{}, month{}, day{}, hour{}, minute{}; };
struct CivilClock { CivilDate date; std::string zone; int offset{}; };
class CivilTime {
  struct Data;
  std::shared_ptr<const Data> data_;
public:
  static CivilTime load(const std::filesystem::path &);
  CivilClock local(Point, std::int64_t utc_ms) const;
  std::vector<std::int64_t> utc(Point, CivilDate) const; // empty=gap; two=fold
  std::optional<std::int64_t> solar(Point, CivilDate, bool sunset) const;
  static std::int64_t milliseconds(CivilDate);
  static CivilDate date(std::int64_t);
};
}
