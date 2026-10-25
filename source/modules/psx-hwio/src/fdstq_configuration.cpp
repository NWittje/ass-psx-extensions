#include "hwio/fdstq.hpp"
#include <cmath>

// Configuration validation is independent of a live Phidget session or SDK.
namespace hwio {
bool fdstq_valid(const FdstqConfiguration& c,std::string& error){
    if(c.serial<0 || (c.enabled && !c.serial) || c.port==0 || c.port>65535 || c.inverted_switches>255 || c.host.size()>253 || c.host.find_first_of("\r\n\t /\\")!=c.host.npos){error="FDSTQ requires an exact positive InterfaceKit serial and a valid Phidget server address/port";return false;}
    for(const auto& id:c.outputs)if(id.size()>256 || (!id.empty() && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")!=id.npos)){error="Invalid FDSTQ output catalogue ID";return false;}
    auto positive=[](double v){return std::isfinite(v) && v>0;};
    for(const auto& m:c.steppers)if(!std::isfinite(m.current) || !std::isfinite(m.acceleration) || !std::isfinite(m.velocity)){error="FDSTQ motor settings must be finite even when disabled";return false;}
    for(const auto v:{c.trim_counts_per_degree,c.trim_velocity,c.trim_acceleration,c.servo_zero,c.servo_full,c.servo_velocity,c.servo_acceleration})if(!std::isfinite(v)){error="FDSTQ trim/servo settings must be finite even when disabled";return false;}
    if(c.hub_serial<0 || c.servo_serial<0 || c.pair_follow>2){error="Invalid FDSTQ motor identities or paired follow policy";return false;}
    for(unsigned n=0;n<3;++n){const auto& m=c.steppers[n];if(m.enabled && (!c.hub_serial || !positive(m.current) || !positive(m.acceleration) || !positive(m.velocity) || m.current>(n<2?1.6:4.))){error="Enabled FDSTQ steppers require exact hub serial, positive velocity/acceleration and bounded current (throttles <=1.6 A, speedbrake <=4 A)";return false;}}
    if(c.trim_enabled && (!c.hub_serial || !std::isfinite(c.trim_counts_per_degree) || c.trim_counts_per_degree==0 || !positive(c.trim_velocity) || c.trim_velocity>1 || !positive(c.trim_acceleration) || c.trim_acceleration>100)){error="Trim motor requires a hub, signed Hall counts per PSX degree, velocity (0,1] and acceleration";return false;}
    if(c.servo_enabled && (!c.servo_serial || !std::isfinite(c.servo_zero) || !std::isfinite(c.servo_full) || c.servo_zero==c.servo_full || c.servo_zero<0 || c.servo_zero>180 || c.servo_full<0 || c.servo_full>180 || !positive(c.servo_velocity) || !positive(c.servo_acceleration))){error="Trim indicator requires exact servo serial, distinct 0/15-degree positions within 0..180 and positive velocity/acceleration";return false;}
    return true;
}
} // namespace hwio
