#pragma once
#include "coordinator/runtime.hpp"
#include <sstream>
namespace coordinator::ui {
std::string describe(const EndpointStatus&,const char*);
void distribution_text(std::ostringstream&,const char*,const psx::bridge::Distribution&);
const char* freshness_name(psx::diagnostics::Freshness);
std::string follow_availability(const ObservationSnapshot&);
std::string timing_text(const ObservationSnapshot&);
void pause_text(std::ostringstream&,const ObservationSnapshot&);
std::string audio_text(const ObservationSnapshot&);
std::string motion_text(const ObservationSnapshot&);
std::string terrain_text(const ObservationSnapshot&);
std::string environment_text(const ObservationSnapshot&);
std::string aircraft_text(const ObservationSnapshot&);
std::string readiness_text(const ObservationSnapshot&);
std::string traffic_text(const ObservationSnapshot&);
}
