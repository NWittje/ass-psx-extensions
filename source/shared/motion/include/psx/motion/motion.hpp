#pragma once
#include <psx/aircraft/state.hpp>
#include <psx/motion/source.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace psx::motion {
inline constexpr std::uint32_t contract_revision=1;
inline constexpr std::uint32_t default_boost_timeout_ms=500;
inline constexpr std::uint32_t relocation_timeout_ms=120000;
inline constexpr double pi=3.1415926535897932384626433832795;
// Value 2 now stops/releases motion without pausing X-Plane. Historical
// capture/source spelling is retained as an alias; no wire layout changes.
enum class Action : std::uint8_t { follow=1, stop=2, stop_pause=stop };
enum class AckResult : std::uint8_t { accepted=1, applied=2, rejected=3, cancelled=4 };
enum class Phase : std::uint8_t { observing, waiting, relocating, following, holding, stopped, fault, stopped_paused=stopped };
enum class Reason : std::uint8_t {
    none, source_unavailable, boost_delayed, invalid_pose, polar_heading, epoch_changed,
    profile_unqualified, binding_unavailable, override_conflict, override_lost,
    competing_writer, pause_failed, relocation_timeout, relocation_failed,
    aircraft_changed, stale_request, cancelled, disconnected, paused_or_frozen,
    scenery_loading, reference_unqualified, transport_overflow, disabled
};
// pause_failed / paused_or_frozen retain their historic numeric meanings for
// capture decoding. Current direct pose output never gates on either pause flag.
enum class Boundary : std::uint8_t { none, attachment, situation_load, initializer, pose_jump, recovery };
enum Flag : std::uint32_t {
    follow_requested=1, override_owned=2, pose_valid=4, approximate_pitch=8,
    placement_starts_engines=16, xp_paused=32, readback_valid=64, profile_valid=128
};
struct Vec3 { double x{},y{},z{}; };
inline Vec3 operator+(Vec3 a,Vec3 b) noexcept {return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 operator-(Vec3 a,Vec3 b) noexcept {return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 operator*(Vec3 a,double b) noexcept {return {a.x*b,a.y*b,a.z*b};}
inline double dot(Vec3 a,Vec3 b) noexcept {return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 cross(Vec3 a,Vec3 b) noexcept {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool finite(Vec3 v) noexcept {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline bool normalize(Vec3 a,Vec3& out) noexcept {
    const double n=std::sqrt(dot(a,a));if(!finite(a)||!std::isfinite(n)||n<1e-10)return false;
    out=a*(1/n);return true;
}
// Column vectors. Body axes and canonical tangent axes are right/up/aft
// and east/up/south respectively. All rotations map body -> scene.
struct Rotation {Vec3 x{1,0,0},y{0,1,0},z{0,0,1};};
inline bool proper_rotation(const Rotation& r) noexcept {
    return finite(r.x)&&finite(r.y)&&finite(r.z)&&std::abs(dot(r.x,r.x)-1)<1e-8&&
        std::abs(dot(r.y,r.y)-1)<1e-8&&std::abs(dot(r.z,r.z)-1)<1e-8&&
        std::abs(dot(r.x,r.y))<1e-8&&std::abs(dot(r.y,r.z))<1e-8&&std::abs(dot(r.z,r.x))<1e-8&&dot(cross(r.x,r.y),r.z)>0;
}
inline Vec3 rotate(const Rotation& r,Vec3 v) noexcept {return r.x*v.x+r.y*v.y+r.z*v.z;}
inline Rotation compose(const Rotation& a,const Rotation& b) noexcept {return {rotate(a,b.x),rotate(a,b.y),rotate(a,b.z)};}
struct GeoPose {double latitude_deg{},longitude_deg{},altitude_msl_m{},heading_deg{},pitch_deg{},bank_deg{};};
// Current PSX internal/Qs121/Boost bank is left-wing-down positive. Canonical
// presentation/SDK bank is right-wing-down positive. Preserve raw observations.
inline double psx_bank_to_right_positive(double source_bank_deg) noexcept {return -source_bank_deg;}
struct LocalPose {Vec3 origin{};double heading_deg{},pitch_deg{},bank_deg{};};
struct SourcePair {
    std::uint64_t session_epoch{},boost_generation{},motion_epoch{},boost_sequence{},pitch_sequence{};
    std::int64_t boost_received_qpc{},pitch_received_qpc{};
};
struct PresentationPose {GeoPose geo{};SourcePair source{};bool on_ground{};PoseProvenance provenance{};};
inline bool valid(const GeoPose& p) noexcept {
    return std::isfinite(p.latitude_deg)&&std::abs(p.latitude_deg)<=90 &&
        std::isfinite(p.longitude_deg)&&std::abs(p.longitude_deg)<=180 &&
        std::isfinite(p.altitude_msl_m)&&std::isfinite(p.heading_deg)&&p.heading_deg>=0&&p.heading_deg<360 &&
        std::isfinite(p.pitch_deg)&&std::abs(p.pitch_deg)<=180 &&
        std::isfinite(p.bank_deg)&&std::abs(p.bank_deg)<=180;
}
inline double wrap_heading(double h) noexcept {h=std::fmod(h,360.0);if(h<0)h+=360;return h>=360?0:h;}
inline double angle_error(double a,double b) noexcept {return std::remainder(a-b,360.0);}
inline Rotation body_rotation(double heading,double pitch,double bank) noexcept {
    const double h=heading*pi/180,p=pitch*pi/180,b=bank*pi/180;
    const double ch=std::cos(h),sh=std::sin(h),cp=std::cos(p),sp=std::sin(p),cb=std::cos(b),sb=std::sin(b);
    return {{cb*ch+sb*sp*sh,-sb*cp,cb*sh-sb*sp*ch},
            {sb*ch-cb*sp*sh,cb*cp,sb*sh+cb*sp*ch}, {-cp*sh,-sp,cp*ch}};
}
// Input directions are differences of SDK WorldToLocal outputs, never a
// locally reimplemented X-Plane map projection. Gram-Schmidt removes roundoff.
inline bool tangent_rotation(Vec3 north,Vec3 up,Rotation& out) noexcept {
    Vec3 u,n,e;if(!normalize(up,u)||!normalize(north-u*dot(north,u),n)||!normalize(cross(n,u),e))return false;
    out={e,u,n*(-1)};return true;
}
inline bool local_pose(const GeoPose& geo,Vec3 reference_local,const Rotation& tangent,Vec3 eye_from_origin,LocalPose& out) noexcept {
    if(!valid(geo)||std::abs(geo.latitude_deg)>89||!finite(reference_local)||!finite(eye_from_origin)||
       !proper_rotation(tangent))return false;
    const auto r=compose(tangent,body_rotation(geo.heading_deg,geo.pitch_deg,geo.bank_deg));
    LocalPose p;p.origin=reference_local-rotate(r,eye_from_origin);
    p.pitch_deg=std::asin((std::max)(-1.0,(std::min)(1.0,-r.z.y)))*180/pi;
    const double cp=std::hypot(r.x.y,r.y.y);
    if(cp>1e-10){p.heading_deg=wrap_heading(std::atan2(-r.z.x,r.z.z)*180/pi);p.bank_deg=std::atan2(-r.x.y,r.y.y)*180/pi;}
    else {p.heading_deg=wrap_heading(std::atan2(r.x.z,r.x.x)*180/pi);p.bank_deg=0;}
    if(!finite(p.origin)||!std::isfinite(p.pitch_deg)||!std::isfinite(p.heading_deg)||!std::isfinite(p.bank_deg))return false;
    out=p;return true;
}
inline bool value(const aircraft::Observation& o,aircraft::PropertyId id,double& out) noexcept {
    for(std::size_t i=0;i<o.count;++i)if(o.values[i].id==id&&o.values[i].element==0&&o.values[i].validity==aircraft::ValueValidity::valid){out=o.values[i].value;return std::isfinite(out);}return false;
}
inline Reason assemble(const aircraft::Observation& boost,const aircraft::Observation& pitch,
                       std::uint64_t session,std::uint64_t boost_generation,std::uint64_t motion_epoch,
                       PresentationPose& out) noexcept {
    using namespace aircraft;
    if(!well_formed(boost)||!well_formed(pitch)||boost.source!=SourceRecord::boost||pitch.source!=SourceRecord::qs121 ||
       boost.position_reference!=PositionReference::psx_flight_deck||pitch.pitch_reference!=PitchReference::horizon)return Reason::invalid_pose;
    if(!session||!boost_generation||!motion_epoch||boost.stamp.session_epoch!=session||pitch.stamp.session_epoch!=session)return Reason::epoch_changed;
    if(boost.heading_reference!=HeadingReference::true_north)return Reason::polar_heading;
    PresentationPose p;double ground{};
    if(!value(boost,PropertyId::latitude_deg,p.geo.latitude_deg)||!value(boost,PropertyId::longitude_deg,p.geo.longitude_deg)||
       !value(boost,PropertyId::altitude_msl_m,p.geo.altitude_msl_m)||!value(boost,PropertyId::heading_deg,p.geo.heading_deg)||
       !value(boost,PropertyId::bank_deg,p.geo.bank_deg)||!value(pitch,PropertyId::pitch_deg,p.geo.pitch_deg)||
       !value(boost,PropertyId::on_ground,ground)||!valid(p.geo))return Reason::invalid_pose;
    if(std::abs(p.geo.latitude_deg)>89)return Reason::polar_heading;
    if(!boost.stamp.sequence||!pitch.stamp.sequence||boost.stamp.received_qpc<0||pitch.stamp.received_qpc<0)return Reason::invalid_pose;
    p.geo.bank_deg=psx_bank_to_right_positive(p.geo.bank_deg);
    p.on_ground=ground==1;p.source={session,boost_generation,motion_epoch,boost.stamp.sequence,pitch.stamp.sequence,boost.stamp.received_qpc,pitch.stamp.received_qpc};
    out=p;return Reason::none;
}
// Spherical distance is only a discontinuity/placement diagnostic, not the
// X-Plane coordinate transform and not a terrain or navigation service.
inline double distance_m(const GeoPose& a,const GeoPose& b) noexcept {
    const double dp=(b.latitude_deg-a.latitude_deg)*pi/180,dl=std::remainder(b.longitude_deg-a.longitude_deg,360.0)*pi/180;
    const double s=std::sin(dp/2),t=std::sin(dl/2);
    const double x=(std::max)(0.0,(std::min)(1.0,s*s+std::cos(a.latitude_deg*pi/180)*std::cos(b.latitude_deg*pi/180)*t*t));
    return 6371008.8*2*std::atan2(std::sqrt(x),std::sqrt(1-x));
}
} // namespace psx::motion
