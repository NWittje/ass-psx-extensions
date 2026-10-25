#include "psx_plugin/terrain_provider.hpp"
#include "psx_plugin/terrain_source_clock.hpp"
#include <psx/sync/runtime.hpp>
#include <XPLMGraphics.h>
#include <cmath>
#include <limits>

namespace psx::xp {
namespace t=terrain;namespace m=motion;
namespace {
bool normalized(m::Vec3 input,m::Vec3& output) noexcept {
    const auto length=std::sqrt(m::dot(input,input));
    if(!m::finite(input)||!std::isfinite(length)||length<1e-9)return false;
    output=input*(1.0/length);return m::finite(output);
}
}
bool TerrainProvider::begin(SdkApi& sdk,m::InstanceId id,std::uint64_t frequency,std::int64_t (*clock)() noexcept) noexcept {
    if(sdk.ready()!=Status::ok||!m::valid(id)||!frequency||!clock)return false;
    retire();sdk_=&sdk;provider_=id;frequency_=frequency;clock_=clock;
    probe_=XPLMCreateProbe(xplm_ProbeY);host_.enabled=probe_!=nullptr;
    ++host_.generation;invalidated_qpc_=clock_();invalidation_reason_=t::Reason::scenery;return host_.enabled;
}
void TerrainProvider::retire() noexcept {
    if(sdk_&&!sdk_->on_thread())return;
    if(probe_&&sdk_&&sdk_->on_thread())XPLMDestroyProbe(probe_);
    probe_=nullptr;host_.enabled=false;++host_.generation;origin_known_=false;probed_=false;profile_={};cg_y_=cg_z_=nullptr;
    last_connection_=last_revision_=last_lease_=0;invalidate_refinement();sdk_=nullptr;
}
void TerrainProvider::invalidate_refinement() noexcept {pending_=seed_known_=false;frozen_={};refinement_={};stations_={};last_query_id_=0;}
bool TerrainProvider::fresh(const t::Reference& r,std::int64_t now) const noexcept {
    return terrain_source_clock::fresh(r,psx::sync::freshness_now(now),frequency_);
}
void TerrainProvider::lifecycle(std::uint64_t aircraft,std::uint64_t scenery,bool loading) noexcept {
    if(!sdk_||!sdk_->on_thread())return;
    if(host_.aircraft_generation!=aircraft){profile_={};cg_y_=cg_z_=nullptr;}
    host_.aircraft_generation=aircraft;host_.scenery_generation=scenery;host_.loading=loading;
    ++host_.generation;invalidated_qpc_=clock_();invalidation_reason_=t::Reason::scenery;last_revision_=0;invalidate_refinement();
}
bool TerrainProvider::local_frame() noexcept {
    // Three non-collinear anchors detect both translation and rotation of the SDK frame.
    m::Vec3 current,north,east;XPLMWorldToLocal(0,0,0,&current.x,&current.y,&current.z);
    XPLMWorldToLocal(0.1,0,0,&north.x,&north.y,&north.z);XPLMWorldToLocal(0,0.1,0,&east.x,&east.y,&east.z);
    if(!m::finite(current)||!m::finite(north)||!m::finite(east))return false;
    const auto changed=[](m::Vec3 a,m::Vec3 b) noexcept{return a.x!=b.x||a.y!=b.y||a.z!=b.z;};
    if(!origin_known_||changed(current,origin_)||changed(north,origin_north_)||changed(east,origin_east_)) {
        origin_=current;origin_north_=north;origin_east_=east;origin_known_=true;++host_.local_frame_generation;++host_.generation;
        invalidated_qpc_=clock_();invalidation_reason_=t::Reason::local_frame;last_revision_=0;invalidate_refinement();
    }
    return true;
}
void TerrainProvider::profile(const MotionProfile& value) noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok||value.generation!=host_.aircraft_generation)return;
    profile_=value;cg_y_=XPLMFindDataRef("sim/aircraft/weight/acf_cgY_original");cg_z_=XPLMFindDataRef("sim/aircraft/weight/acf_cgZ_original");
    ++host_.generation;invalidated_qpc_=clock_();invalidation_reason_=t::Reason::identity;last_revision_=0;invalidate_refinement();
}
bool TerrainProvider::geometry_valid() noexcept {
    if(!profile_.qualified||profile_.generation!=host_.aircraft_generation||!profile_.footprint.qualified||
       profile_.footprint.id!=t::sparky_footprint_geometry_id||profile_.footprint.revision!=t::footprint_geometry_revision||
       !m::finite(profile_.eye)||sdk_->inspect(cg_y_,xplmType_Float,false)!=Status::ok||sdk_->inspect(cg_z_,xplmType_Float,false)!=Status::ok)return false;
    for(const auto& p:profile_.footprint.centres)if(!m::finite(p))return false;
    const double y=XPLMGetDataf(cg_y_),z=XPLMGetDataf(cg_z_);
    return std::isfinite(y)&&std::isfinite(z)&&std::abs(y-profile_.cg_y_ft)<.01&&std::abs(z-profile_.cg_z_ft)<.01;
}
bool TerrainProvider::freeze(const TerrainInput& input,TerrainResult& result) noexcept {
    const auto& r=input.query.reference;m::GeoPose eye;if(!t::reference_eye(r,eye))return false;
    m::Vec3 point,north,up;
    XPLMWorldToLocal(eye.latitude_deg,eye.longitude_deg,eye.altitude_msl_m,&point.x,&point.y,&point.z);
    XPLMWorldToLocal(eye.latitude_deg+.0001,eye.longitude_deg,eye.altitude_msl_m,&north.x,&north.y,&north.z);
    XPLMWorldToLocal(eye.latitude_deg,eye.longitude_deg,eye.altitude_msl_m+10,&up.x,&up.y,&up.z);
    m::Rotation tangent;if(!m::tangent_rotation(north-point,up-point,tangent))return false;
    const auto rotation=m::compose(tangent,m::body_rotation(eye.heading_deg,eye.pitch_deg,eye.bank_deg));
    result.surface.geometry_id=profile_.footprint.id;result.surface.geometry_revision=profile_.footprint.revision;
    for(unsigned i=0;i<5;++i) {
        auto& station=stations_[i];station={};station.target=input.query;auto& target=station.target.reference;
        const auto local=point+m::rotate(rotation,profile_.footprint.centres[i]-profile_.eye);
        if(!m::finite(local))return false;
        XPLMLocalToWorld(local.x,local.y,local.z,&target.latitude_deg,&target.longitude_deg,&target.altitude_msl_m);
        if(!t::valid(target))return false;
        t::GeographicOffset offset;if(!t::geographic_offset(r.latitude_deg,r.longitude_deg,target.latitude_deg,target.longitude_deg,offset))return false;
        station.east=offset.east_m;station.north=offset.north_m;
        station.cold=!seed_known_;station.seed=seed_known_?warm_seed(target,i):target.altitude_msl_m;
        auto& d=result.details[i];d.query=input.query;d.provider=provider_;d.capture_generation=input.capture_active?input.capture_generation:0;
        d.aircraft_generation=host_.aircraft_generation;d.scenery_generation=host_.scenery_generation;d.local_frame_generation=host_.local_frame_generation;
        d.latitude_deg=target.latitude_deg;d.longitude_deg=target.longitude_deg;
        d.geometry_id=profile_.footprint.id;d.geometry_revision=profile_.footprint.revision;d.assembly_index=i;
    }
    return true;
}
double TerrainProvider::warm_seed(const t::Reference& r,unsigned i) noexcept {
    // Predict the previous qualified plane at this geographic reference using SDK
    // differentials. This is only a probe seed, never a substituted terrain hit.
    m::Vec3 point,up;const auto h=seeds_[i].height_msl_m;
    XPLMWorldToLocal(r.latitude_deg,r.longitude_deg,h,&point.x,&point.y,&point.z);
    XPLMWorldToLocal(r.latitude_deg,r.longitude_deg,h+1,&up.x,&up.y,&up.z);
    const auto denominator=m::dot(up-point,seed_normals_[i]);
    if(!m::finite(point)||!m::finite(up)||!std::isfinite(denominator)||denominator<=1e-9){stations_[i].cold=true;return r.altitude_msl_m;}
    const auto seed=h+m::dot(seed_hits_[i]-point,seed_normals_[i])/denominator;
    if(std::isfinite(seed)&&seed>=-20000&&seed<=100000)return seed;
    stations_[i].cold=true;return r.altitude_msl_m;
}
bool TerrainProvider::query(const t::Query& request,double seed,t::Surface& out,m::Vec3& hit_point,m::Vec3& hit_normal,double& horizontal,t::ProbeAttempt& detail,XPLMProbeRef probe) noexcept {
    detail={};detail.seed_msl_m=seed;detail.query_start_qpc=clock_();
    const auto& r=request.reference;m::Vec3 point;
    XPLMWorldToLocal(r.latitude_deg,r.longitude_deg,seed,&point.x,&point.y,&point.z);
    if(!m::finite(point)||std::abs(point.x)>std::numeric_limits<float>::max()||
       std::abs(point.y)>std::numeric_limits<float>::max()||std::abs(point.z)>std::numeric_limits<float>::max())return false;
    detail.start_local=point;
    XPLMProbeInfo_t hit{};hit.structSize=sizeof(hit);
    // Recompute all three coordinates at every geographic fixed-point attempt.
    detail.sdk_result=static_cast<std::uint32_t>(XPLMProbeTerrainXYZ(probe?probe:probe_,static_cast<float>(point.x),static_cast<float>(point.y),static_cast<float>(point.z),&hit));
    detail.query_end_qpc=clock_();detail.hit_local={hit.locationX,hit.locationY,hit.locationZ};detail.normal_local={hit.normalX,hit.normalY,hit.normalZ};
    if(detail.sdk_result!=xplm_ProbeHitTerrain)return false;
    detail.flags=1;
    out.flags=t::hit|(hit.is_wet?t::wet:0u);
    const m::Vec3 velocity{hit.velocityX,hit.velocityY,hit.velocityZ};
    if(!m::finite(velocity))return false;
    if(m::dot(velocity,velocity)>0)out.flags|=t::moving;
    XPLMLocalToWorld(hit.locationX,hit.locationY,hit.locationZ,&out.latitude_deg,&out.longitude_deg,&out.height_msl_m);
    detail.hit_msl_m=out.height_msl_m;detail.height_residual_m=std::abs(seed-out.height_msl_m);
    if(!std::isfinite(out.latitude_deg)||!std::isfinite(out.longitude_deg)||!std::isfinite(out.height_msl_m))return false;
    m::Vec3 at,north,up;
    XPLMWorldToLocal(out.latitude_deg,out.longitude_deg,out.height_msl_m,&at.x,&at.y,&at.z);
    XPLMWorldToLocal(out.latitude_deg+0.0001,out.longitude_deg,out.height_msl_m,&north.x,&north.y,&north.z);
    XPLMWorldToLocal(out.latitude_deg,out.longitude_deg,out.height_msl_m+10,&up.x,&up.y,&up.z);
    m::Rotation tangent;m::Vec3 normal;
    if(!m::tangent_rotation(north-at,up-at,tangent)||!normalized({hit.normalX,hit.normalY,hit.normalZ},normal))return false;
    out.normal_east=m::dot(normal,tangent.x);out.normal_north=-m::dot(normal,tangent.z);out.normal_up=m::dot(normal,tangent.y);
    hit_point={hit.locationX,hit.locationY,hit.locationZ};hit_normal=normal;
    m::Vec3 reference;
    XPLMWorldToLocal(r.latitude_deg,r.longitude_deg,out.height_msl_m,&reference.x,&reference.y,&reference.z);
    // Residual is horizontal in the native geographic tangent at returned MSL,
    // not distance to the elevated starting point or local-X/Z coincidence.
    m::Vec3 ref_north,ref_up;
    XPLMWorldToLocal(r.latitude_deg+0.0001,r.longitude_deg,out.height_msl_m,&ref_north.x,&ref_north.y,&ref_north.z);
    XPLMWorldToLocal(r.latitude_deg,r.longitude_deg,out.height_msl_m+10,&ref_up.x,&ref_up.y,&ref_up.z);
    m::Rotation ref_tangent;if(!m::tangent_rotation(ref_north-reference,ref_up-reference,ref_tangent))return false;
    const auto difference=hit_point-reference;
    horizontal=std::hypot(m::dot(difference,ref_tangent.x),m::dot(difference,ref_tangent.z));
    detail.horizontal_residual_m=horizontal;
    if(!std::isfinite(horizontal))return false;
    return std::isfinite(out.height_msl_m)&&out.normal_up>0;
}
bool TerrainProvider::tick(const TerrainInput& input,TerrainResult& output) noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok||!host_.enabled)return false;
    const auto now=clock_();const bool frame_valid=local_frame();
    if(input.connection!=last_connection_||input.lease_generation!=last_lease_) {
        last_connection_=input.connection;last_lease_=input.lease_generation;last_revision_=0;invalidate_refinement();
    }
    if(domain_.owner!=input.control.owner||domain_.terrain_epoch!=input.control.terrain_epoch||domain_.producer!=input.control.producer||domain_.pose_epoch!=input.control.pose_epoch) {
        domain_=input.control;last_revision_=0;invalidate_refinement();
    }
    if(!input.connection||!input.active||now>=input.lease_expires_qpc)invalidate_refinement();
    if(pending_&&(frozen_.query.owner!=input.control.owner||frozen_.query.terrain_epoch!=input.control.terrain_epoch||
        frozen_.query.reference.producer!=input.control.producer||frozen_.query.reference.pose_epoch!=input.control.pose_epoch))invalidate_refinement();
    if(!input.have_query||!input.connection)return false;
    if(!pending_&&(input.revision==last_revision_||input.query.query_id<=last_query_id_))return false;
    const auto& selected=pending_?frozen_:input;
    TerrainResult result;result.connection=input.connection;result.revision=input.revision;result.host_generation=host_.generation;result.lease_generation=input.lease_generation;
    if(pending_)result=refinement_;
    auto& surface=result.surface;surface.query=selected.query;surface.provider=provider_;
    surface.aircraft_generation=host_.aircraft_generation;surface.scenery_generation=host_.scenery_generation;
    surface.local_frame_generation=host_.local_frame_generation;
    if(!pending_)surface.query_start_qpc=now;
    surface.query_end_qpc=now;
    surface.latitude_deg=selected.query.reference.latitude_deg;surface.longitude_deg=selected.query.reference.longitude_deg;
    result.reason=t::Reason::none;
    if(!input.active)result.reason=t::Reason::stopped;
    else if(now>=input.lease_expires_qpc)result.reason=t::Reason::expired;
    else if(selected.query.owner!=input.control.owner||selected.query.terrain_epoch!=input.control.terrain_epoch||
        selected.query.reference.producer!=input.control.producer||selected.query.reference.pose_epoch!=input.control.pose_epoch)result.reason=t::Reason::epoch;
    else if(!t::valid(selected.query.reference))result.reason=t::Reason::invalid_surface;
    else if(host_.loading)result.reason=t::Reason::loading;
    else if(!frame_valid)result.reason=t::Reason::local_frame;
    else if(!terrain_source_clock::after_cutoff(selected.query.reference.received_qpc,invalidated_qpc_,
        psx::sync::enabled()?psx::sync::clock_margin_ticks():0))result.reason=invalidation_reason_;
    else if(!fresh(selected.query.reference,now)||now<surface.query_start_qpc||
        now-surface.query_start_qpc>=static_cast<std::int64_t>(frequency_*t::stale_ms/1000))result.reason=t::Reason::stale;
    else if(!geometry_valid())result.reason=t::Reason::identity;
    else {
        const auto interval=static_cast<std::int64_t>((frequency_+99)/100);
        if(probed_&&now>=last_probe_&&now-last_probe_<interval)return false;
        last_probe_=now;probed_=true;
        if(!pending_) {
            frozen_=input;if(!freeze(input,result))result.reason=t::Reason::invalid_surface;
            else pending_=true;
        }
        // One physical attempt per unfinished station, at most five per callback.
        // Accepted station results remain frozen until this bundle completes.
        for(unsigned i=0;i<5&&result.reason==t::Reason::none;++i) {
            auto& station=stations_[i];if(station.complete)continue;
            const auto before=clock_();
            if(!fresh(surface.query.reference,before)||before<surface.query_start_qpc||
               before-surface.query_start_qpc>=static_cast<std::int64_t>(frequency_*t::stale_ms/1000)||before>=input.lease_expires_qpc){result.reason=t::Reason::stale;break;}
            auto& detail=result.details[i];auto& attempt=detail.attempts[detail.count++];detail.valid_mask=(1u<<detail.count)-1;
            result.attempts=(std::max)(result.attempts,detail.count);
            t::Surface hit;hit.query=station.target;hit.provider=provider_;hit.query_start_qpc=hit.query_end_qpc=before;
            double horizontal{};
            const bool good=query(station.target,station.seed,hit,station.hit,station.normal,horizontal,attempt)&&t::valid(hit)&&!(hit.flags&(t::wet|t::moving));
            const auto end=clock_();attempt.query_end_qpc=(std::max)(end,attempt.query_start_qpc);
            result.height_residual_m=(std::max)(result.height_residual_m,attempt.height_residual_m);
            result.horizontal_residual_m=(std::max)(result.horizontal_residual_m,horizontal);
            if(end<before)result.reason=t::Reason::invalid_surface;
            else if(!fresh(surface.query.reference,end)||end-surface.query_start_qpc>=static_cast<std::int64_t>(frequency_*t::stale_ms/1000)||end>=input.lease_expires_qpc)result.reason=t::Reason::stale;
            else if(!good){surface.flags|=hit.flags&(t::wet|t::moving);result.reason=t::Reason::invalid_surface;}
            else if((!station.cold||detail.count>1)&&attempt.height_residual_m<=.05&&horizontal<=.05) {
                auto& s=surface.stations[i];const auto& target=station.target.reference;
                s.latitude_deg=target.latitude_deg;s.longitude_deg=target.longitude_deg;s.height_msl_m=hit.height_msl_m;
                s.east_m=station.east;s.north_m=station.north;s.normal_east=hit.normal_east;s.normal_north=hit.normal_north;s.normal_up=hit.normal_up;
                s.query_start_qpc=detail.attempts[0].query_start_qpc;s.query_end_qpc=end;
                surface.station_mask|=1u<<i;station.complete=true;attempt.flags|=2;
                result.height_residual_m=attempt.height_residual_m;result.horizontal_residual_m=horizontal;
            } else if(detail.count==4)result.reason=t::Reason::invalid_surface;
            else station.seed=hit.height_msl_m;
            attempt.reason=result.reason;
        }
        surface.query_end_qpc=clock_();
        if(result.reason==t::Reason::none&&surface.station_mask!=31){refinement_=result;return false;}
        if(result.reason==t::Reason::none) {
            surface.flags=t::hit|t::geometry_qualified|t::footprint_plane;
            if(!t::fit_footprint(surface)||!t::qualified_footprint(surface))result.reason=t::Reason::invalid_surface;
            result.height_residual_m=result.horizontal_residual_m=0;
            for(const auto& d:result.details){const auto& a=d.attempts[d.count-1];result.height_residual_m=(std::max)(result.height_residual_m,a.height_residual_m);result.horizontal_residual_m=(std::max)(result.horizontal_residual_m,a.horizontal_residual_m);}
        }
    }
    surface.query_end_qpc=clock_();
    if(surface.query_end_qpc<surface.query_start_qpc){surface.query_end_qpc=surface.query_start_qpc;surface.flags=0;result.reason=t::Reason::invalid_surface;}
    if(result.reason==t::Reason::none&&(!fresh(surface.query.reference,surface.query_end_qpc)||
        surface.query_end_qpc-surface.query_start_qpc>=static_cast<std::int64_t>(frequency_*t::stale_ms/1000)||surface.query_end_qpc>=input.lease_expires_qpc))result.reason=t::Reason::stale;
    if(result.reason!=t::Reason::none){surface.flags&=~(t::hit|t::geometry_qualified|t::footprint_plane);seed_known_=false;surface.station_mask=0;}
    else {
        for(unsigned i=0;i<5;++i){seeds_[i].height_msl_m=surface.stations[i].height_msl_m;seed_hits_[i]=stations_[i].hit;seed_normals_[i]=stations_[i].normal;}
        seed_known_=true;
    }
    for(auto& detail:result.details)if(detail.count&&result.reason!=t::Reason::none){auto& a=detail.attempts[detail.count-1];a.reason=result.reason;a.flags&=~2u;}
    pending_=false;last_revision_=result.revision;
    // A stopped/expired control cycle has not consumed an active query.
    if(input.active&&clock_()<input.lease_expires_qpc)last_query_id_=surface.query.query_id;
    output=result;return true;
}
}
