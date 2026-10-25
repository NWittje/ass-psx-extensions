#pragma once
#include <psx/motion/reconstruction.hpp>

namespace psx::motion {
inline constexpr std::uint32_t exporter_schema_revision=1;
enum RawPoseFlag : std::uint32_t {
    raw_pose_valid=1,raw_on_ground=2,raw_grid_north=4,raw_grid_south=8,
    raw_ordinary_altitude=16,raw_paused_altitude=32,raw_normalization=64,
    raw_transition=128,raw_guard_rejected=256
};
// A raw copy, not a native wire ABI. Java binary32 fields remain binary32.
struct RawPose {
    std::uint64_t sequence{},frame_sequence{},pose_epoch{};
    std::int64_t sample_start_qpc{},sample_end_qpc{},scheduler_wall_ms{};
    double delta_ms{},delta_acc_ms{},simulation_rate{};
    std::uint32_t flags{};
    double latitude_rad{},longitude_rad{},heading_rad{};
    float total_pitch_rad{},bank_rad{},altitude_ft{},main_strut_ft{},base_pitch_rad{};
    float nose_osc_rad{},nose_compress_rad{},cached_sin_pitch{},stock_eye_altitude_ft{};
    float longitudinal_heading_rad{},fpv_rad{},slope_fade{};
    double stock_eye_latitude_rad{},stock_eye_longitude_rad{};
};
// Hamilton scalar-first, active body right/up/aft -> fixed Earth XYZ.
struct Quaternion {double w{1},x{},y{},z{};};
inline double norm_squared(const Quaternion& q) noexcept {return q.w*q.w+q.x*q.x+q.y*q.y+q.z*q.z;}
inline bool valid(const Quaternion& q) noexcept {
    return std::isfinite(q.w)&&std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::abs(norm_squared(q)-1)<1e-6;
}
inline bool normalize(Quaternion& q) noexcept {
    const auto n=norm_squared(q);if(!std::isfinite(n)||n<1e-20)return false;
    const auto inv=1/std::sqrt(n);q.w*=inv;q.x*=inv;q.y*=inv;q.z*=inv;return true;
}
inline Quaternion quaternion(const Rotation& r) noexcept {
    Quaternion q;const double t=r.x.x+r.y.y+r.z.z;
    if(t>0){const auto s=std::sqrt(t+1)*2;q={s/4,(r.y.z-r.z.y)/s,(r.z.x-r.x.z)/s,(r.x.y-r.y.x)/s};}
    else if(r.x.x>r.y.y&&r.x.x>r.z.z){const auto s=std::sqrt(1+r.x.x-r.y.y-r.z.z)*2;q={(r.y.z-r.z.y)/s,s/4,(r.y.x+r.x.y)/s,(r.z.x+r.x.z)/s};}
    else if(r.y.y>r.z.z){const auto s=std::sqrt(1+r.y.y-r.x.x-r.z.z)*2;q={(r.z.x-r.x.z)/s,(r.y.x+r.x.y)/s,s/4,(r.z.y+r.y.z)/s};}
    else {const auto s=std::sqrt(1+r.z.z-r.x.x-r.y.y)*2;q={(r.x.y-r.y.x)/s,(r.z.x+r.x.z)/s,(r.z.y+r.y.z)/s,s/4};}
    normalize(q);return q;
}
inline Rotation rotation(const Quaternion& q) noexcept {
    const auto w=q.w,x=q.x,y=q.y,z=q.z;
    return {{1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y)},
            {2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x)},
            {2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)}};
}
inline Rotation transpose(const Rotation& r) noexcept {return {{r.x.x,r.y.x,r.z.x},{r.x.y,r.y.y,r.z.y},{r.x.z,r.y.z,r.z.z}};}
inline Rotation earth_tangent(double latitude_deg,double longitude_deg) noexcept {
    const auto p=latitude_deg*pi/180,l=longitude_deg*pi/180;
    const auto sp=std::sin(p),cp=std::cos(p),sl=std::sin(l),cl=std::cos(l);
    return {{-sl,cl,0},{cp*cl,cp*sl,sp},{sp*cl,sp*sl,-cp}};
}
inline Quaternion slerp(Quaternion a,Quaternion b,double t) noexcept {
    double d=a.w*b.w+a.x*b.x+a.y*b.y+a.z*b.z;
    if(d<0){b={-b.w,-b.x,-b.y,-b.z};d=-d;}
    d=(std::max)(0.0,(std::min)(1.0,d));double u=1-t,v=t;
    if(d<0.9995){const auto angle=std::acos(d),s=std::sin(angle);u=std::sin((1-t)*angle)/s;v=std::sin(t*angle)/s;}
    Quaternion q{u*a.w+v*b.w,u*a.x+v*b.x,u*a.y+v*b.y,u*a.z+v*b.z};normalize(q);return q;
}
inline double orientation_error_deg(const Quaternion& a,const Quaternion& b) noexcept {
    const auto d=std::abs(a.w*b.w+a.x*b.x+a.y*b.y+a.z*b.z);
    return 2*std::acos((std::min)(1.0,d))*180/pi;
}
struct CompletePoseSample {
    std::uint64_t sequence{},frame_sequence{},configuration_epoch{};
    std::int64_t sample_start_qpc{},sample_end_qpc{},received_qpc{};
    double latitude_deg{},longitude_deg{},altitude_msl_m{};
    Quaternion attitude{};
    std::uint32_t flags{};
};
struct CompletePoseWindow {
    PoseDomain domain{};
    std::uint64_t revision{},sequence_floor{};
    std::uint16_t count{};
    std::array<CompletePoseSample,history_capacity> samples{};
};
inline bool valid(const CompletePoseSample& s) noexcept {
    constexpr std::uint32_t permitted=raw_pose_valid|raw_on_ground|raw_ordinary_altitude|raw_paused_altitude;
    const auto branch=s.flags&(raw_ordinary_altitude|raw_paused_altitude);
    return s.sequence&&s.frame_sequence&&s.sample_start_qpc>=0&&s.sample_end_qpc>=s.sample_start_qpc&&s.received_qpc>=0&&
        // Cross-thread QPC comparisons can differ by one tick.
        (s.received_qpc>=s.sample_end_qpc || s.sample_end_qpc-s.received_qpc==1)&&
        std::isfinite(s.latitude_deg)&&std::abs(s.latitude_deg)<=89&&
        std::isfinite(s.longitude_deg)&&std::abs(s.longitude_deg)<=180&&std::isfinite(s.altitude_msl_m)&&valid(s.attitude)&&
        (s.flags&raw_pose_valid)&&!(s.flags&~permitted)&&(branch==raw_ordinary_altitude||branch==raw_paused_altitude);
}
inline bool valid(const CompletePoseWindow& w) noexcept {
    if(!valid(w.domain)||!w.revision||w.count>history_capacity)return false;
    for(std::size_t i=0;i<w.count;++i){const auto& s=w.samples[i];if(!valid(s)||s.sequence<w.sequence_floor)return false;
        if(i&&(s.sequence<=w.samples[i-1].sequence||s.frame_sequence<=w.samples[i-1].frame_sequence||s.sample_end_qpc<w.samples[i-1].sample_end_qpc||s.received_qpc<w.samples[i-1].received_qpc||
               (s.flags&(raw_ordinary_altitude|raw_paused_altitude))!=(w.samples[i-1].flags&(raw_ordinary_altitude|raw_paused_altitude))))return false;}
    return true;
}
inline bool finite(const RawPose& r) noexcept {
    const double values[]={r.delta_ms,r.delta_acc_ms,r.simulation_rate,r.latitude_rad,r.longitude_rad,r.heading_rad,
        r.total_pitch_rad,r.bank_rad,r.altitude_ft,r.main_strut_ft,r.base_pitch_rad,r.nose_osc_rad,r.nose_compress_rad,
        r.cached_sin_pitch,r.stock_eye_altitude_ft,r.longitudinal_heading_rad,r.fpv_rad,r.slope_fade,r.stock_eye_latitude_rad,r.stock_eye_longitude_rad};
    for(const auto v:values)if(!std::isfinite(v))return false;return true;
}
inline bool valid(const RawPose& r) noexcept {
    return r.sequence&&r.frame_sequence&&r.pose_epoch&&r.sample_start_qpc>=0&&r.sample_end_qpc>=r.sample_start_qpc&&finite(r)&&
        !(r.flags&~511U)&&std::abs(r.latitude_rad)<=pi/2&&std::abs(r.longitude_rad)<=pi&&
        r.delta_ms>=0&&r.delta_acc_ms>=0&&r.simulation_rate>=0;
}
// Qualified PSX visual recipe, not an anatomical rigid-body datum.
inline bool derive_pose(const RawPose& r,std::int64_t received_qpc,std::uint64_t configuration_epoch,CompletePoseSample& out,
                        std::uint32_t reference_revision=instant_reference_revision) noexcept {
    if(!known_reference_revision(reference_revision)||!valid(r)||!(r.flags&raw_pose_valid)||(r.flags&(raw_grid_north|raw_grid_south|raw_normalization|raw_transition|raw_guard_rejected))||std::abs(r.latitude_rad)>89*pi/180)return false;
    const auto branch=r.flags&(raw_ordinary_altitude|raw_paused_altitude);
    if(branch!=raw_ordinary_altitude&&branch!=raw_paused_altitude)return false;
    constexpr double d=4.42834879647753e-6;
    const double lat=std::asin((std::max)(-1.0,(std::min)(1.0,std::sin(r.latitude_rad)*std::cos(d)+std::cos(r.latitude_rad)*std::sin(d)*std::cos(r.heading_rad))));
    const double lon=std::remainder(r.longitude_rad+std::atan2(std::sin(r.heading_rad)*std::sin(d)*std::cos(r.latitude_rad),std::cos(d)-std::sin(r.latitude_rad)*std::sin(lat)),2*pi);
    const double strut=branch==raw_ordinary_altitude?r.main_strut_ft:0.0;
    CompletePoseSample s;s.sequence=r.sequence;s.frame_sequence=r.frame_sequence;s.configuration_epoch=configuration_epoch;
    s.sample_start_qpc=r.sample_start_qpc;s.sample_end_qpc=r.sample_end_qpc;s.received_qpc=received_qpc;
    s.latitude_deg=lat*180/pi;s.longitude_deg=lon*180/pi;
    s.altitude_msl_m=(static_cast<double>(r.altitude_ft)+static_cast<double>(28.412073F)+92.5*std::sin(static_cast<double>(r.total_pitch_rad))-strut)*0.3048;
    // Promote before arithmetic in the current contract; retain historical rounding for replay.
    const auto raw_bank_deg=reference_revision==instant_reference_revision?static_cast<double>(r.bank_rad)*180/pi:r.bank_rad*180/pi;
    const auto bank_deg=reference_revision==instant_reference_revision?psx_bank_to_right_positive(raw_bank_deg):raw_bank_deg;
    const auto pitch_deg=reference_revision==instant_reference_revision?static_cast<double>(r.total_pitch_rad)*180/pi:r.total_pitch_rad*180/pi;
    s.attitude=quaternion(compose(earth_tangent(s.latitude_deg,s.longitude_deg),body_rotation(wrap_heading(r.heading_rad*180/pi),pitch_deg,bank_deg)));
    s.flags=r.flags;if(!valid(s))return false;out=s;return true;
}
inline bool geographic_pose(const CompletePoseSample& s,GeoPose& out) noexcept {
    if(!valid(s))return false;
    const auto r=compose(transpose(earth_tangent(s.latitude_deg,s.longitude_deg)),rotation(s.attitude));
    GeoPose p;p.latitude_deg=s.latitude_deg;p.longitude_deg=s.longitude_deg;p.altitude_msl_m=s.altitude_msl_m;
    p.pitch_deg=std::asin((std::max)(-1.0,(std::min)(1.0,-r.z.y)))*180/pi;
    if(std::hypot(r.x.y,r.y.y)>1e-10){p.heading_deg=wrap_heading(std::atan2(-r.z.x,r.z.z)*180/pi);p.bank_deg=std::atan2(-r.x.y,r.y.y)*180/pi;}
    else {p.heading_deg=wrap_heading(std::atan2(r.x.z,r.x.x)*180/pi);p.bank_deg=0;}
    if(!valid(p))return false;out=p;return true;
}
struct CompleteHistoryCounters {std::uint64_t accepted{},evicted{},equal_time{},rejected{},resets{};};
class CompletePoseHistory {
public:
    const CompletePoseWindow& window() const noexcept{return window_;}
    const CompleteHistoryCounters& counters() const noexcept{return counters_;}
    void synchronize(const PoseDomain& domain,std::uint64_t floor=0) noexcept {
        if(domain!=window_.domain||floor!=window_.sequence_floor){const auto revision=window_.revision+1;window_={};window_.revision=revision;window_.domain=domain;window_.sequence_floor=floor;++counters_.resets;}
        if(!window_.revision)window_.revision=1;
    }
    void clear() noexcept {const auto revision=window_.revision+1;window_={};window_.revision=revision;++counters_.resets;}
    bool append(const CompletePoseSample& s) noexcept {
        if(!valid(window_.domain)||!valid(s)||s.sequence<window_.sequence_floor){++counters_.rejected;return false;}
        if(window_.count){const auto& p=window_.samples[window_.count-1];
            if(s.sequence<=p.sequence||s.frame_sequence<=p.frame_sequence||s.sample_end_qpc<p.sample_end_qpc||s.received_qpc<p.received_qpc){++counters_.rejected;return false;}
            if(s.sample_end_qpc==p.sample_end_qpc)++counters_.equal_time;
            // Producer branch changes alter the reference recipe; never cross them.
            if((s.flags&(raw_ordinary_altitude|raw_paused_altitude))!=(p.flags&(raw_ordinary_altitude|raw_paused_altitude))){window_.count=0;++counters_.resets;}
        }
        if(window_.count==history_capacity){for(std::size_t i=1;i<history_capacity;++i)window_.samples[i-1]=window_.samples[i];--window_.count;++counters_.evicted;}
        window_.samples[window_.count++]=s;++window_.revision;++counters_.accepted;return true;
    }
    bool restore_capture_checkpoint(const CompletePoseWindow& w,const CompleteHistoryCounters& c) noexcept {
        if(w.count&&!valid(w))return false;if(w.count>history_capacity)return false;window_=w;counters_=c;return true;
    }
private:CompletePoseWindow window_{};CompleteHistoryCounters counters_{};
};
inline bool reconstruct(const CompletePoseWindow& w,const ReconstructionSettings& settings,std::int64_t now,std::uint64_t frequency,
                        std::int64_t cursor,ReconstructionResult& out,std::int64_t future_bound=0) noexcept {
    out={};out.trace.window_revision=w.revision;
    if(!valid(w)||!w.count||!valid(settings)||!frequency||frequency>1000000000000ULL||now<0||cursor<0||cursor>now)return false;
    const auto& newest=w.samples[w.count-1];if(future_bound<0||(newest.sample_end_qpc>now&&newest.sample_end_qpc-now>future_bound)){out.trace.state=ReconstructionState::invalid_timing;return false;}
    const auto delay=static_cast<std::int64_t>(frequency*settings.delay_ms/1000);
    const auto wanted=settings.mode==ReconstructionMode::direct?now:(now>=delay?now-delay:0);
    out.trace.target_qpc=wanted;const auto target=(std::max)(wanted,cursor);
    std::size_t left=0,right=0;double alpha=0;
    if(settings.mode==ReconstructionMode::direct){left=right=w.count-1;out.trace.state=ReconstructionState::direct;}
    else if(target<w.samples[0].sample_end_qpc){
        while(left+1<w.count&&w.samples[left+1].sample_end_qpc==w.samples[0].sample_end_qpc)++left;
        right=left;out.trace.state=ReconstructionState::oldest_hold;
    }
    else {
        // Last sample at an equal timestamp wins; no fabricated timing.
        for(std::size_t i=0;i<w.count&&w.samples[i].sample_end_qpc<=target;++i)left=i;
        right=left;
        if(left+1<w.count){right=left+1;
            while(right+1<w.count&&w.samples[right+1].sample_end_qpc==w.samples[right].sample_end_qpc)++right;
            const auto span=w.samples[right].sample_end_qpc-w.samples[left].sample_end_qpc;
            if(span<=0)return false;alpha=static_cast<double>(target-w.samples[left].sample_end_qpc)/static_cast<double>(span);out.trace.state=ReconstructionState::interpolated;
        }else out.trace.state=ReconstructionState::newest_hold;
    }
    const auto& a=w.samples[left];const auto& b=w.samples[right];auto p=a;
    if(left!=right){p.latitude_deg=a.latitude_deg+(b.latitude_deg-a.latitude_deg)*alpha;p.longitude_deg=std::remainder(a.longitude_deg+std::remainder(b.longitude_deg-a.longitude_deg,360.0)*alpha,360.0);
        p.altitude_msl_m=a.altitude_msl_m+(b.altitude_msl_m-a.altitude_msl_m)*alpha;p.attitude=slerp(a.attitude,b.attitude,alpha);}
    if(!geographic_pose(p,out.pose.geo))return false;
    out.pose.on_ground=(p.flags&raw_on_ground)!=0;
    out.pose.provenance.kind=PoseSource::exporter;out.pose.provenance.domain=w.domain;
    out.pose.provenance.left_sequence=a.sequence;out.pose.provenance.right_sequence=b.sequence;out.pose.provenance.frame_sequence=a.frame_sequence;
    out.pose.provenance.sample_start_qpc=a.sample_start_qpc;out.pose.provenance.sample_end_qpc=a.sample_end_qpc;out.pose.provenance.received_qpc=a.received_qpc;
    out.trace.left_sequence=a.sequence;out.trace.right_sequence=b.sequence;out.trace.left_received_qpc=a.received_qpc;out.trace.right_received_qpc=b.received_qpc;out.trace.alpha=alpha;
    out.trace.presentation_qpc=left==right?a.sample_end_qpc:target;
    // Cursor must never silently move backward within a domain.
    if(out.trace.presentation_qpc<cursor){out.trace.state=ReconstructionState::invalid_timing;return false;}
    return true;
}
} // namespace psx::motion
