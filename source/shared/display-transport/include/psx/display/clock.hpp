#pragma once
#include "transport.hpp"
#include <filesystem>
namespace psx::du::transport {
struct ClockSample {uint64_t t1{},t2{},t3{},t4{},client_frequency{},host_frequency{};double offset_ms{},uncertainty_ms{};};
// Four timestamps establish an interval, not synchronized clocks. No clock is changed.
ClockSample clock_sample(const std::string& host,const std::string& adapter);
void clock_server(const std::string& adapter,unsigned seconds);
}
