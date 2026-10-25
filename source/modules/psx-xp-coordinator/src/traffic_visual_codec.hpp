#pragma once
#include "coordinator/traffic_visual.hpp"
#include <vector>
namespace coordinator::traffic_visual_capture {
// Canonical explicit little-endian schema. Serialization is a writer/replay duty,
// never an acquisition/SDK duty. The state is chunked into <=4096-byte envelopes.
bool encode(const TrafficVisualModel::State&,std::vector<std::uint8_t>&);
bool encode(const TrafficVisualView&,std::vector<std::uint8_t>&);
bool encode(const TrafficVisualEvent&,std::vector<std::uint8_t>&);
bool decode(const std::vector<std::uint8_t>&,TrafficVisualModel::State&);
bool decode(const std::vector<std::uint8_t>&,TrafficVisualView&);
bool decode(const std::vector<std::uint8_t>&,TrafficVisualEvent&);
bool equal(const TrafficVisualView&,const TrafficVisualView&);
}
