#pragma once
#include "core.hpp"
#include <psx/instructor/airport_database.hpp>
#include <filesystem>
#include <memory>
namespace ios {
using Point = psx::instructor::Point;
struct RunwayEnd : psx::instructor::Runway { std::size_t opposite{}; };
struct Glidepath {
  std::string runway, id;
  Point transmitter;
  double angle{};
  int elevation{};
};
struct Airport {
  std::string id, name;
  Point reference;
  int elevation{};
  std::vector<RunwayEnd> runways;
  std::vector<Glidepath> glidepaths;
  bool eligible() const;
};
struct Catalogue {
  std::filesystem::path folder;
  std::string identity;
  std::array<std::string, 3> hashes;
  std::map<std::string, Airport> airports;
  static std::shared_ptr<const Catalogue> load(const std::filesystem::path &);
  const Airport &airport(std::string_view) const;
  std::string search(std::string_view, std::optional<Point> center = {}) const;
  std::string detail(std::string_view) const;
};
struct Target {
  std::string airport, runway, preset, source;
  Point position;
  double heading{}, altitude{}, height{}, ias{}, distance{};
  double pitch_deg{}, bank_deg{}; // Airborne attitude; the simple ground sender uses zero angles.
  int threshold_elevation{};
  bool ground{}, manual{};
  std::string json() const;
};
std::string number(double);
std::string airport_id(std::string_view, bool prefix = false);
Point destination(Point, double heading, double distance_nm);
double distance_nm(Point, Point);
Target target(const Catalogue &, std::string_view airport, std::string_view runway,
              std::string_view preset, double ias, bool manual, double height);
std::string sha256(std::string_view);
} // namespace ios
