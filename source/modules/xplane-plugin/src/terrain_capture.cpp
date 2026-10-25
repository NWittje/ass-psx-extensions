#include "psx_plugin/terrain_capture.hpp"
#include "psx_plugin/terrain_source_clock.hpp"
#include <psx/sync/runtime.hpp>
#include <XPLMCamera.h>
#include <XPLMGraphics.h>
#include <cmath>
#include <limits>

namespace psx::xp {
namespace t=terrain;namespace m=motion;
bool TerrainCapture::begin(TerrainProvider& p) noexcept {
    retire();if(!p.sdk_||p.sdk_->ready()!=Status::ok)return false;provider_=&p;
    probe_=XPLMCreateProbe(xplm_ProbeY);
    constexpr const char* positions[]={"sim/flightmodel/position/local_x","sim/flightmodel/position/local_y","sim/flightmodel/position/local_z"};
    constexpr const char* angles[]={"sim/flightmodel/position/theta","sim/flightmodel/position/psi","sim/flightmodel/position/phi"};
    for(unsigned i=0;i<3;++i){xyz_[i]=XPLMFindDataRef(positions[i]);angles_[i]=XPLMFindDataRef(angles[i]);}
    return probe_!=nullptr;
}
void TerrainCapture::retire() noexcept {
    if(provider_&&provider_->sdk_&&!provider_->sdk_->on_thread())return;
    if(probe_&&provider_&&provider_->sdk_)XPLMDestroyProbe(probe_);
    provider_=nullptr;probe_=nullptr;xyz_={};angles_={};clear();sampled_=false;connection_=lease_=capture_=host_=0;
}
bool TerrainCapture::tick(const TerrainInput& input,TerrainCaptureResult& output) noexcept {
    if(!provider_||!provider_->sdk_||provider_->sdk_->ready()!=Status::ok)return false;
    auto& p=*provider_;const auto now=p.clock_();
    if(connection_!=input.connection||lease_!=input.lease_generation||capture_!=input.capture_generation||host_!=p.host_.generation) {
        clear();connection_=input.connection;lease_=input.lease_generation;capture_=input.capture_generation;host_=p.host_.generation;
    }
    if(domain_.owner!=input.control.owner||domain_.terrain_epoch!=input.control.terrain_epoch||domain_.pose_epoch!=input.control.pose_epoch||domain_.producer!=input.control.producer){domain_=input.control;clear();}
    if(!input.capture_active||!input.capture_generation||!input.active||!input.connection||!input.have_query||p.host_.loading||
        now>=input.lease_expires_qpc||!p.fresh(input.query.reference,now)) {clear();return false;}
    if(input.query.owner!=input.control.owner||input.query.terrain_epoch!=input.control.terrain_epoch||input.query.reference.producer!=input.control.producer||
        input.query.reference.pose_epoch!=input.control.pose_epoch||!terrain_source_clock::after_cutoff(input.query.reference.received_qpc,p.invalidated_qpc_,
        psx::sync::enabled()?psx::sync::clock_margin_ticks():0)) {clear();return false;}
    const auto interval=static_cast<std::int64_t>((p.frequency_+99)/100);
    if(sampled_&&now>=last_tick_&&now-last_tick_<interval)return false;
    sampled_=true;last_tick_=now;output={};output.connection=input.connection;output.lease_generation=input.lease_generation;output.host_generation=p.host_.generation;
    auto& r=output.render;r.query=input.query;r.provider=p.provider_;r.capture_generation=input.capture_generation;r.sample_qpc=now;
    r.aircraft_generation=p.host_.aircraft_generation;r.scenery_generation=p.host_.scenery_generation;r.local_frame_generation=p.host_.local_frame_generation;
    double positions[3]{},angles[3]{};bool aircraft=true;
    for(unsigned i=0;i<3;++i) {
        if(p.sdk_->inspect(xyz_[i],xplmType_Double,false)!=Status::ok||p.sdk_->inspect(angles_[i],xplmType_Float,false)!=Status::ok){aircraft=false;continue;}
        positions[i]=XPLMGetDatad(xyz_[i]);angles[i]=XPLMGetDataf(angles_[i]);
        aircraft=aircraft&&std::isfinite(positions[i])&&std::isfinite(angles[i]);
    }
    if(aircraft){r.aircraft_local={positions[0],positions[1],positions[2]};r.aircraft_angles_deg={angles[0],angles[1],angles[2]};r.valid_mask|=1;}
    XPLMCameraPosition_t camera;
    const auto invalid=std::numeric_limits<float>::quiet_NaN();camera={invalid,invalid,invalid,invalid,invalid,invalid,invalid};
    XPLMReadCameraPosition(&camera);
    const m::Vec3 camera_position{camera.x,camera.y,camera.z},camera_angles{camera.pitch,camera.heading,camera.roll};
    if(m::finite(camera_position)&&m::finite(camera_angles)) {
        XPLMLocalToWorld(camera.x,camera.y,camera.z,&r.camera_latitude_deg,&r.camera_longitude_deg,&r.camera_altitude_msl_m);
        auto reference=input.query.reference;reference.latitude_deg=r.camera_latitude_deg;reference.longitude_deg=r.camera_longitude_deg;reference.altitude_msl_m=r.camera_altitude_msl_m;
        if(t::valid(reference)){r.camera_local=camera_position;r.camera_angles_deg=camera_angles;r.valid_mask|=2;}
    }
    output.have_render=true;
    auto& check=checks_[next_];const unsigned index=next_;next_^=1;
    if(check.pending&&(!p.fresh(check.detail.query.reference,now)||check.detail.query.owner!=input.control.owner||
        check.detail.query.terrain_epoch!=input.control.terrain_epoch||check.detail.query.reference.pose_epoch!=input.control.pose_epoch||
        check.detail.query.reference.producer!=input.control.producer)){check={};}
    if(!check.pending) {
        if(index==1&&!(r.valid_mask&2))return true;
        check.target=input.query;
        if(index==1){check.target.reference.latitude_deg=r.camera_latitude_deg;check.target.reference.longitude_deg=r.camera_longitude_deg;check.target.reference.altitude_msl_m=r.camera_altitude_msl_m;}
        check.detail={};auto& d=check.detail;d.query=input.query;d.provider=p.provider_;d.capture_generation=input.capture_generation;
        d.aircraft_generation=p.host_.aircraft_generation;d.scenery_generation=p.host_.scenery_generation;d.local_frame_generation=p.host_.local_frame_generation;
        d.purpose=index==0?t::ProbePurpose::reference_check:t::ProbePurpose::camera_ground;
        d.latitude_deg=check.target.reference.latitude_deg;d.longitude_deg=check.target.reference.longitude_deg;
        check.cold=!check.warm;check.seed=check.warm?check.warm_height:check.target.reference.altitude_msl_m;check.pending=true;
    }
    if(!probe_){check={};return true;}
    auto& d=check.detail;auto& attempt=d.attempts[d.count++];d.valid_mask=(1u<<d.count)-1;
    t::Surface surface;surface.query=check.target;surface.provider=p.provider_;surface.query_start_qpc=surface.query_end_qpc=now;
    m::Vec3 point,normal;double residual{};
    bool good=p.query(check.target,check.seed,surface,point,normal,residual,attempt,probe_)&&t::valid(surface)&&!(surface.flags&(t::wet|t::moving));
    const auto end=p.clock_();attempt.query_end_qpc=end;
    if(end<now){attempt.query_end_qpc=now;attempt.reason=t::Reason::invalid_surface;good=false;}
    else if(!p.fresh(d.query.reference,end)||end>=input.lease_expires_qpc){attempt.reason=t::Reason::stale;good=false;}
    else if(!good)attempt.reason=t::Reason::invalid_surface;
    const bool qualified=good&&(!check.cold||d.count>1)&&attempt.height_residual_m<=.05&&residual<=.05;
    if(qualified){attempt.flags|=2;check.warm=true;check.warm_height=surface.height_msl_m;}
    if(qualified||!good||d.count==4) {
        if(good&&!qualified)attempt.reason=t::Reason::invalid_surface;
        if(!qualified)check.warm=false;
        output.probe=d;output.have_probe=true;check.pending=false;
    } else check.seed=surface.height_msl_m;
    return true;
}
}
