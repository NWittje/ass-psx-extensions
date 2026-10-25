#pragma once
#include "traffic_model.hpp"
namespace psx::xp {
// Worker only. Use the generator's exact common root spelling; no disk path lookup.
inline bool actor_load_paths(TrafficModel& model,const std::filesystem::path& root)noexcept{
 return prepare_traffic_load_paths(model,root);
}
}
