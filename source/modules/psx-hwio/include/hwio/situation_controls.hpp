#pragma once
#include "axis_setup.hpp"
#include "button_setup.hpp"
#include <psx/hwio/situation_controls.hpp>
namespace hwio {
// Runtime-thread reader only: no SDK access, filtering or command submission.
class SituationControls {
public:
    static psx::hwio::situation_controls::Snapshot snapshot(const Mapper&,
        const AxisRouter&,const ButtonRouter&,const SourceBank&,
        const std::vector<DeviceInfo>&,unsigned bench_throttles,
        std::int64_t qpc,std::uint64_t frequency,const std::vector<Profile>& bench_sources={});
};
}
