#include "hwio/fdstq_motion.hpp"
#include "hwio/hardware_router.hpp"
#include "hwio/motor_motion.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
namespace hwio {
std::array<psx::hwio::thrust_control::Lever,4> fdstq_thrust_status(const FdstqConfiguration& c,const AxisConfiguration& axes,const Mapper& mapper,std::uint64_t generation,const std::array<FdstqDriveStatus,2>& states,bool enabled){
    namespace t=psx::hwio::thrust_control;std::array<t::Lever,4> result;FdstqThrust probe;probe.owned=probe.drive=true;
    const auto motion=fdstq_motion(c,axes,mapper,generation,&probe);const auto key="fdstq-phidget22:"+std::to_string(c.serial);
    for(unsigned i=0;i<4;++i){auto& l=result[i];l.master=i;if(!c.enabled || (!c.paired && i>=2))continue;const unsigned n=c.paired?i/2:i,master=c.paired?2*n:n;
        const auto& row=axes.rows[9+i];if(!c.steppers[n].enabled || !row.managed || !row.active || row.applied.source.identity.hardware_key!=key)continue;
        const auto& state=states[n];l.master=master;l.group=101+n;l.flags=t::covered|(state.drive==MotorDriveState::off?t::drive_released:0);l.generation=generation+1;l.operation_id=state.operation;
        l.calibration=1469598103934665603ull;auto mix=[&](std::uint64_t x){l.calibration=(l.calibration^x)*1099511628211ull;};mix(unsigned(c.serial));mix(unsigned(c.hub_serial));mix(c.steppers[n].reversed);for(const auto x:row.applied.points)mix(std::bit_cast<std::uint64_t>(double(x)));mix(row.applied.captured);mix(std::bit_cast<std::uint64_t>(c.steppers[n].velocity));mix(std::bit_cast<std::uint64_t>(c.steppers[n].acceleration));mix(std::bit_cast<std::uint64_t>(c.steppers[n].current));
        std::string reason;
        if(!enabled)reason="FDS/FDSTQ provider disabled";else if(!generation || !state.available || state.generation!=generation)reason="FDSTQ throttle drive unavailable";
        else if(!motion.enabled[n])reason="FDSTQ throttle assignment/forward calibration unavailable";
        else if(!state.valid)reason="FDSTQ physical feedback unavailable";else if(state.drive==MotorDriveState::unknown || !state.reason.empty())reason=state.reason.empty()?"FDSTQ drive release unconfirmed":state.reason;
        else l.flags|=t::ready;
        const auto& source=axes.rows[9+master].applied.source;
        if(state.valid && motion.full[n]!=motion.zero[n] && source.bindings.size()==1){l.flags|=t::physical_valid;l.physical_percent=100*normalize(state.raw,source.bindings[0].calibration);}
        l.phase=!(l.flags&t::ready)?t::Phase::unavailable:state.limited?t::Phase::limit:state.held?t::Phase::held:state.drive==MotorDriveState::on?t::Phase::positioning:t::Phase::ready;
        std::copy_n(reason.begin(),std::min(reason.size(),l.reason.size()-1),l.reason.begin());
    }return result;
}
bool fdstq_speed_write(const AxisConfiguration &axes, const Mapper &m, const Command &command) noexcept {
    const auto &row = axes.rows[8];
    if (!row.managed || !row.active || !row.applied.source.identity.hardware_key.starts_with("fdstq-phidget22:"))
        return false;
    const auto *role = flight_target(8);
    return role && std::any_of(m.mappings.begin(), m.mappings.end(), [&](const auto &route) {
               return route.externally_calculated && route.binding.id == command.binding &&
                      route.binding.target.id == role->descriptor.id;
           });
}
void FdstqSpeedFollow::written(int value) noexcept {
    if (overflow_ || end_ - begin_ == echoes_.size()) {
        overflow_ = true;
        following_ = false;
        return;
    }
    echoes_[end_++ % echoes_.size()] = value;
}
bool FdstqSpeedFollow::update(FdstqMotion &m, bool available, double feedback, int observed, std::uint64_t sequence,
                              std::uint64_t epoch) noexcept {
    if (!m.enabled[2] || !available || !std::isfinite(feedback)) {
        reset();
        m.speed_follow = false;
        return false;
    }
    if (generation_ != m.generation || epoch_ != epoch) {
        reset();
        generation_ = m.generation;
        epoch_ = epoch;
    }
    if (overflow_) {
        m.speed_follow = false;
        return false;
    }
    if (!known_) {
        known_ = true;
        observed_ = observed;
        sequence_ = sequence;
    } else if (sequence_ != sequence) {
        bool own = false;
        for (auto n = begin_; n < end_; ++n)
            if (echoes_[n % echoes_.size()] == observed) {
                begin_ = n + 1;
                own = true;
            }
        if (observed != observed_ && !own)
            following_ = true;
        observed_ = observed;
        sequence_ = sequence;
    }
    // Release input ownership within 0.3% of the captured ADC travel, matching
    // the motor velocity deadband below rather than a fixed count tolerance.
    if (std::abs(m.target[2] - feedback) <= std::abs(m.full[2] - m.zero[2]) * .003)
        following_ = false;
    m.speed_follow = following_;
    return following_;
}
double fdstq_velocity(double target, double feedback, double zero, double full, bool reversed, double limit) noexcept {
    if (!std::isfinite(target) || !std::isfinite(feedback) || !std::isfinite(zero) || !std::isfinite(full) ||
        !std::isfinite(limit) || limit <= 0 || zero == full)
        return 0;
    const double lo = std::min(zero, full), hi = std::max(zero, full);
    // Target and feedback are ADC counts. Permit feedback only within the
    // calibrated travel plus 2% at either end; invalid feedback stops motion.
    if (target < lo || target > hi)
        return 0;
    const int edge=feedback<lo?-1:feedback>hi?1:0;
    if(edge && !motor_inward(feedback,target,edge))return 0;
    const double error = (target - feedback) / (full - zero);
    // The error is a fraction of calibrated travel; 0.3% requests no motion.
    if (std::abs(error) <= .003)
        return 0;
    // Scale the configured Phidget stepper velocity limit by this dimensionless
    // error; the result uses motor velocity units, not ADC counts per second.
    return std::clamp(error * 10., -1., 1.) * limit * (reversed ? -1. : 1.);
}
FdstqMotion fdstq_motion(const FdstqConfiguration &c, const AxisConfiguration &axes, const Mapper &m,
                         std::uint64_t generation,const FdstqThrust* ios) {
    FdstqMotion out;
    out.generation = generation;
    const bool external=ios && ios->owned;
    if ((!m.ready && !external) || !generation || !c.enabled)
        return out;
    out.psx_epoch = m.epoch;
    out.ios_thrust=external;if(external){out.ios_token=ios->token;out.ios_operation=ios->operation;}
    auto observation = [&](std::string_view id, double &value) {
        const auto o = active_catalogue().observations.find(id);
        if (o == active_catalogue().observations.end())
            return false;
        const auto r = m.records.find(record_name(o->second->record));
        std::string_view text;
        return r != m.records.end() && observation_value(*o->second, r->second.observed, value, text) &&
               std::floor(value) == value;
    };
    const auto hardware_key = "fdstq-phidget22:" + std::to_string(c.serial);
    static constexpr std::string_view thrust_ids[]{"observe.tla1", "observe.tla2", "observe.tla3", "observe.tla4"};
    auto axis = [&](unsigned slot, unsigned channel, bool follower) {
        const auto &row = axes.rows[slot];
        const auto *definition = flight_target(slot);
        if (!definition || !definition->descriptor.writable || !row.managed || !row.active ||
            row.applied.source.identity.hardware_key != hardware_key || row.applied.source.bindings.size() != 1)
            return false;
        const auto &b = row.applied.source.bindings[0];
        const unsigned captures = b.calibration.centered ? 7u : 5u;
        return b.enabled && (row.applied.captured & captures) == captures && flight_binding_matches(slot, b) &&
               b.sources.size() == 1 && b.sources[0].key.channel == channel && calibration_valid(b.calibration) &&
               (follower ? b.throttle == ThrottlePolicy::at_servo_follower : !b.calibration.centered);
    };
    for (unsigned n = 0; n < 3; ++n) {
        const unsigned slot = n == 2 ? 8 : 9 + (n == 1 ? (c.paired ? 2 : 1) : 0);
        const auto &row = axes.rows[slot];
        if (!c.steppers[n].enabled || !axis(slot, n + 1, n < 2))
            continue;
        double normalized{};
        if (n < 2) {
            if (!external && m.at != AtAuthority::servo)
                continue;
            double a{}, b{};
            const unsigned engine = slot - 8;
            if(external)a=ios->percent[engine-1]*50;
            else if (!observation(thrust_ids[engine - 1], a) || a < 0)
                continue;
            if (c.paired) {
                if (!axis(slot + 1, n + 1, true) || (!external && (!observation(thrust_ids[engine], b) || b < 0)))
                    continue;
                if (!external && c.pair_follow == 2 && std::abs(a - b) > 25)
                    continue;
                if (!external && c.pair_follow == 1)
                    a = (a + b) / 2.;
            }
            normalized = a / 5000.;
        } else {
            if(!m.ready)continue;
            double value{};
            if (!observation("observe.fdstq.speedbrake", value))
                continue;
            normalized = value / 800.;
        }
        if (normalized < 0 || normalized > 1)
            continue;
        out.zero[n] =
            row.applied.source.bindings[0].calibration.centered ? row.applied.points[1] : row.applied.points[0];
        out.full[n] = row.applied.points[2];
        const auto &calibration = row.applied.source.bindings[0].calibration;
        const double fraction = calibration.inverted ? 1 - normalized : normalized;
        out.target[n] =
            normalized == 0   ? out.zero[n]
            : normalized == 1 ? out.full[n]
            : calibration.centered
                ? (calibration.inverted ? calibration.center - calibration.dead_center_low -
                                              normalized * (calibration.center - calibration.dead_center_low -
                                                            calibration.minimum - calibration.dead_minimum)
                                        : calibration.center + calibration.dead_center_high +
                                              normalized * (calibration.maximum - calibration.dead_maximum -
                                                            calibration.center - calibration.dead_center_high))
                : calibration.minimum + calibration.dead_minimum +
                      fraction * (calibration.maximum - calibration.dead_maximum - calibration.minimum -
                                  calibration.dead_minimum);
        out.enabled[n] = n>=2 || !external || ios->drive;
    }
    double trim{}, power{};
    if (m.ready && observation("observe.fdstq.stabtrim", trim)) {
        out.trim_degrees = trim / 1000.;
        out.trim = c.trim_enabled;
        if (observation("observe.fdstq.trim-power", power))
            out.servo = c.servo_enabled && (static_cast<unsigned>(power) & 1) != 0;
    }
    return out;
}
} // namespace hwio
