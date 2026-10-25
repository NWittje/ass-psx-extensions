#include <psx/terrain/protocol.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace psx::terrain {
namespace {
struct Writer {
 std::uint8_t* data;std::size_t size,pos{};bool ok{true};
 void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(8*i));}
 void real(double v) noexcept {std::uint64_t b{};std::memcpy(&b,&v,8);u(b,8);}
};
struct Reader {
 const std::uint8_t* data;std::size_t size,pos{};bool ok{true};std::uint16_t version{protocol_version};
 std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(8*i);return v;}
 std::int64_t signed64() noexcept {const auto b=u(8);std::int64_t v{};std::memcpy(&v,&b,8);return v;}
 double real() noexcept {const auto b=u(8);double v{};std::memcpy(&v,&b,8);return v;}
 bool end() const noexcept {return ok&&pos==size;}
};
bool finite(double v) noexcept {return std::isfinite(v);}
bool geographic(double lat,double lon,double height) noexcept {return finite(lat)&&finite(lon)&&finite(height)&&std::abs(lat)<=89&&std::abs(lon)<=180&&height>=-20000&&height<=100000;}
bool normal(double e,double n,double u) noexcept {return finite(e)&&finite(n)&&finite(u)&&u>0&&std::abs(e*e+n*n+u*u-1)<=1e-5;}
bool valid(const Hello& v) noexcept {return (v.role==Role::plugin||v.role==Role::exporter)&&motion::valid(v.instance)&&v.pid&&v.process_creation_filetime&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL&&(v.capabilities&~511u)==0;}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.instance)&&v.owner&&v.pid&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
bool valid(const Query& v) noexcept {return v.owner&&v.terrain_epoch&&v.query_id&&terrain::valid(v.reference);}
bool valid(const Control& v) noexcept {return motion::valid(v.producer)&&v.owner&&v.operation&&v.terrain_epoch&&v.pose_epoch&&static_cast<unsigned>(v.action)>=1&&static_cast<unsigned>(v.action)<=3&&v.duration_ms==lease_ms;}
bool valid(const Ack& v) noexcept {return valid(v.control)&&static_cast<unsigned>(v.phase)<=4&&static_cast<unsigned>(v.reason)<=13;}
bool valid(const Consumed& v) noexcept {
 return motion::valid(v.producer)&&v.pose_epoch&&v.consumed_qpc>0&&static_cast<unsigned>(v.phase)<=4&&static_cast<unsigned>(v.reason)<=13&&
 geographic(v.latitude_deg,v.longitude_deg,v.effective_height_msl_m)&&normal(v.normal_east,v.normal_north,v.normal_up)&&
 finite(v.support_pitch_rad)&&finite(v.support_bank_left_rad)&&finite(v.strut_ft)&&finite(v.nose_osc_rad)&&finite(v.nose_compress_rad);
}
void put(Writer& w,motion::InstanceId v) noexcept {w.u(v.high,8);w.u(v.low,8);}
void get(Reader& r,motion::InstanceId& v) noexcept {v.high=r.u(8);v.low=r.u(8);}
void put(Writer& w,const Hello& v) noexcept {w.u(static_cast<std::uint64_t>(v.role),4);put(w,v.instance);w.u(static_cast<std::uint64_t>(v.pid),4);w.u(static_cast<std::uint64_t>(v.capabilities),4);w.u(static_cast<std::uint64_t>(v.process_creation_filetime),8);w.u(static_cast<std::uint64_t>(v.qpc_frequency),8);}
void get(Reader& r,Hello& v) noexcept {v.role=static_cast<decltype(v.role)>(r.u(4));get(r,v.instance);v.pid=static_cast<decltype(v.pid)>(r.u(4));v.capabilities=static_cast<decltype(v.capabilities)>(r.u(4));v.process_creation_filetime=static_cast<decltype(v.process_creation_filetime)>(r.u(8));v.qpc_frequency=static_cast<decltype(v.qpc_frequency)>(r.u(8));}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.instance);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.qpc_frequency),8);w.u(static_cast<std::uint64_t>(v.pid),4);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.instance);v.owner=static_cast<decltype(v.owner)>(r.u(8));v.qpc_frequency=static_cast<decltype(v.qpc_frequency)>(r.u(8));v.pid=static_cast<decltype(v.pid)>(r.u(4));}
void put(Writer& w,const Reference& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.pose_epoch),8);w.u(static_cast<std::uint64_t>(v.sequence),8);w.u(static_cast<std::uint64_t>(v.frame_sequence),8);w.u(static_cast<std::uint64_t>(v.sample_qpc),8);w.u(static_cast<std::uint64_t>(v.received_qpc),8);w.real(v.latitude_deg);w.real(v.longitude_deg);w.real(v.altitude_msl_m);w.real(v.heading_rad);w.real(v.pitch_rad);w.real(v.bank_left_rad);w.u(static_cast<std::uint64_t>(v.flags),4);w.real(v.main_strut_ft);w.u(v.reference_revision,4);}
void get(Reader& r,Reference& v) noexcept {get(r,v.producer);v.pose_epoch=static_cast<decltype(v.pose_epoch)>(r.u(8));v.sequence=static_cast<decltype(v.sequence)>(r.u(8));v.frame_sequence=static_cast<decltype(v.frame_sequence)>(r.u(8));v.sample_qpc=r.signed64();v.received_qpc=r.signed64();v.latitude_deg=r.real();v.longitude_deg=r.real();v.altitude_msl_m=r.real();v.heading_rad=r.real();v.pitch_rad=r.real();v.bank_left_rad=r.real();v.flags=static_cast<decltype(v.flags)>(r.u(4));if(r.version>=3){v.main_strut_ft=r.real();v.reference_revision=static_cast<std::uint32_t>(r.u(4));}}
void put(Writer& w,const Query& v) noexcept {w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.terrain_epoch),8);w.u(static_cast<std::uint64_t>(v.query_id),8);put(w,v.reference);}
void get(Reader& r,Query& v) noexcept {v.owner=static_cast<decltype(v.owner)>(r.u(8));v.terrain_epoch=static_cast<decltype(v.terrain_epoch)>(r.u(8));v.query_id=static_cast<decltype(v.query_id)>(r.u(8));get(r,v.reference);}
void put(Writer& w,const Surface& v) noexcept {put(w,v.query);put(w,v.provider);w.u(static_cast<std::uint64_t>(v.aircraft_generation),8);w.u(static_cast<std::uint64_t>(v.scenery_generation),8);w.u(static_cast<std::uint64_t>(v.local_frame_generation),8);w.u(static_cast<std::uint64_t>(v.query_start_qpc),8);w.u(static_cast<std::uint64_t>(v.query_end_qpc),8);w.real(v.latitude_deg);w.real(v.longitude_deg);w.real(v.height_msl_m);w.real(v.normal_east);w.real(v.normal_north);w.real(v.normal_up);w.u(static_cast<std::uint64_t>(v.flags),4);put(w,v.geometry_id);w.u(v.geometry_revision,4);w.u(v.station_mask,4);for(const auto& a:v.stations){w.real(a.latitude_deg);w.real(a.longitude_deg);w.real(a.height_msl_m);w.real(a.east_m);w.real(a.north_m);w.real(a.normal_east);w.real(a.normal_north);w.real(a.normal_up);w.real(a.plane_residual_m);w.u(static_cast<std::uint64_t>(a.query_start_qpc),8);w.u(static_cast<std::uint64_t>(a.query_end_qpc),8);}}
void get(Reader& r,Surface& v) noexcept {get(r,v.query);get(r,v.provider);v.aircraft_generation=static_cast<decltype(v.aircraft_generation)>(r.u(8));v.scenery_generation=static_cast<decltype(v.scenery_generation)>(r.u(8));v.local_frame_generation=static_cast<decltype(v.local_frame_generation)>(r.u(8));v.query_start_qpc=r.signed64();v.query_end_qpc=r.signed64();v.latitude_deg=r.real();v.longitude_deg=r.real();v.height_msl_m=r.real();v.normal_east=r.real();v.normal_north=r.real();v.normal_up=r.real();v.flags=static_cast<decltype(v.flags)>(r.u(4));if(r.version>=3){get(r,v.geometry_id);v.geometry_revision=static_cast<std::uint32_t>(r.u(4));v.station_mask=static_cast<std::uint32_t>(r.u(4));for(auto& a:v.stations){a.latitude_deg=r.real();a.longitude_deg=r.real();a.height_msl_m=r.real();a.east_m=r.real();a.north_m=r.real();a.normal_east=r.real();a.normal_north=r.real();a.normal_up=r.real();a.plane_residual_m=r.real();a.query_start_qpc=r.signed64();a.query_end_qpc=r.signed64();}}}
void put(Writer& w,const Control& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.operation),8);w.u(static_cast<std::uint64_t>(v.terrain_epoch),8);w.u(static_cast<std::uint64_t>(v.pose_epoch),8);w.u(static_cast<std::uint64_t>(v.action),4);w.u(static_cast<std::uint64_t>(v.duration_ms),4);}
void get(Reader& r,Control& v) noexcept {get(r,v.producer);v.owner=static_cast<decltype(v.owner)>(r.u(8));v.operation=static_cast<decltype(v.operation)>(r.u(8));v.terrain_epoch=static_cast<decltype(v.terrain_epoch)>(r.u(8));v.pose_epoch=static_cast<decltype(v.pose_epoch)>(r.u(8));v.action=static_cast<decltype(v.action)>(r.u(4));v.duration_ms=static_cast<decltype(v.duration_ms)>(r.u(4));}
void put(Writer& w,const Ack& v) noexcept {put(w,v.control);w.u(static_cast<std::uint64_t>(v.phase),4);w.u(static_cast<std::uint64_t>(v.reason),4);w.u(static_cast<std::uint64_t>(v.accepted_query),8);}
void get(Reader& r,Ack& v) noexcept {get(r,v.control);v.phase=static_cast<decltype(v.phase)>(r.u(4));v.reason=static_cast<decltype(v.reason)>(r.u(4));v.accepted_query=static_cast<decltype(v.accepted_query)>(r.u(8));}
void put(Writer& w,const Consumed& v) noexcept {put(w,v.producer);w.u(static_cast<std::uint64_t>(v.terrain_epoch),8);w.u(static_cast<std::uint64_t>(v.pose_epoch),8);w.u(static_cast<std::uint64_t>(v.frame_sequence),8);w.u(static_cast<std::uint64_t>(v.query_id),8);w.u(static_cast<std::uint64_t>(v.owner),8);w.u(static_cast<std::uint64_t>(v.consumed_qpc),8);w.u(static_cast<std::uint64_t>(v.surface_qpc),8);w.u(static_cast<std::uint64_t>(v.phase),4);w.u(static_cast<std::uint64_t>(v.reason),4);w.real(v.latitude_deg);w.real(v.longitude_deg);w.real(v.effective_height_msl_m);w.real(v.normal_east);w.real(v.normal_north);w.real(v.normal_up);w.real(v.support_pitch_rad);w.real(v.support_bank_left_rad);w.real(v.strut_ft);w.real(v.nose_osc_rad);w.real(v.nose_compress_rad);w.u(static_cast<std::uint64_t>(v.support_configuration),4);w.u(static_cast<std::uint64_t>(v.contact_flags),4);w.u(static_cast<std::uint64_t>(v.applied),8);w.u(static_cast<std::uint64_t>(v.rejected),8);w.u(static_cast<std::uint64_t>(v.hook_ticks),8);w.u(static_cast<std::uint64_t>(v.source_sample_qpc),8);}
void get(Reader& r,Consumed& v) noexcept {get(r,v.producer);v.terrain_epoch=static_cast<decltype(v.terrain_epoch)>(r.u(8));v.pose_epoch=static_cast<decltype(v.pose_epoch)>(r.u(8));v.frame_sequence=static_cast<decltype(v.frame_sequence)>(r.u(8));v.query_id=static_cast<decltype(v.query_id)>(r.u(8));v.owner=static_cast<decltype(v.owner)>(r.u(8));v.consumed_qpc=r.signed64();v.surface_qpc=r.signed64();v.phase=static_cast<decltype(v.phase)>(r.u(4));v.reason=static_cast<decltype(v.reason)>(r.u(4));v.latitude_deg=r.real();v.longitude_deg=r.real();v.effective_height_msl_m=r.real();v.normal_east=r.real();v.normal_north=r.real();v.normal_up=r.real();v.support_pitch_rad=r.real();v.support_bank_left_rad=r.real();v.strut_ft=r.real();v.nose_osc_rad=r.real();v.nose_compress_rad=r.real();v.support_configuration=static_cast<decltype(v.support_configuration)>(r.u(4));v.contact_flags=static_cast<decltype(v.contact_flags)>(r.u(4));v.applied=static_cast<decltype(v.applied)>(r.u(8));v.rejected=static_cast<decltype(v.rejected)>(r.u(8));v.hook_ticks=static_cast<decltype(v.hook_ticks)>(r.u(8));v.source_sample_qpc=r.signed64();}
bool valid(const Diagnostics& v) noexcept {
 if(!valid(v.query)||!motion::valid(v.provider)||(v.valid_mask&~31u))return false;
 for(std::size_t i=0;i<5;++i){const auto& a=v.assemblies[i];if(static_cast<unsigned>(a.reason)>13)return false;
  if(v.valid_mask&(1u<<i)){if(a.reason!=Reason::none||a.query_start_qpc<=0||a.query_end_qpc<a.query_start_qpc||!finite(a.support_point_msl_m)||!finite(a.surface_msl_m)||!finite(a.clearance_m))return false;}
 }return true;
}
void put(Writer& w,const Diagnostics& v) noexcept {
 put(w,v.query);put(w,v.provider);w.u(v.aircraft_generation,8);w.u(v.scenery_generation,8);w.u(v.local_frame_generation,8);w.u(v.valid_mask,4);
 for(const auto& a:v.assemblies){w.u(static_cast<std::uint64_t>(a.query_start_qpc),8);w.u(static_cast<std::uint64_t>(a.query_end_qpc),8);w.real(a.support_point_msl_m);w.real(a.surface_msl_m);w.real(a.clearance_m);w.u(static_cast<unsigned>(a.reason),4);}
}
void get(Reader& r,Diagnostics& v) noexcept {
 get(r,v.query);get(r,v.provider);v.aircraft_generation=r.u(8);v.scenery_generation=r.u(8);v.local_frame_generation=r.u(8);v.valid_mask=static_cast<std::uint32_t>(r.u(4));
 for(auto& a:v.assemblies){a.query_start_qpc=r.signed64();a.query_end_qpc=r.signed64();a.support_point_msl_m=r.real();a.surface_msl_m=r.real();a.clearance_m=r.real();a.reason=static_cast<Reason>(r.u(4));}
}
void put(Writer& w,motion::Vec3 v) noexcept {w.real(v.x);w.real(v.y);w.real(v.z);}
void get(Reader& r,motion::Vec3& v) noexcept {v.x=r.real();v.y=r.real();v.z=r.real();}
bool valid(const Capture& v) noexcept {return v.owner&&v.generation&&v.enabled<=1;}
void put(Writer& w,const Capture& v) noexcept {w.u(v.owner,8);w.u(v.generation,8);w.u(v.enabled,4);}
void get(Reader& r,Capture& v) noexcept {v.owner=r.u(8);v.generation=r.u(8);v.enabled=static_cast<std::uint32_t>(r.u(4));}
bool valid(const ProbeDetail& v) noexcept {
 if(!valid(v.query)||!motion::valid(v.provider)||!v.capture_generation||static_cast<unsigned>(v.purpose)>2||
    !v.count||v.count>4||v.valid_mask!=((1u<<v.count)-1)||!geographic(v.latitude_deg,v.longitude_deg,0))return false;
 for(std::size_t i=0;i<v.count;++i){const auto& a=v.attempts[i];
  if(a.query_start_qpc<=0||a.query_end_qpc<a.query_start_qpc||a.sdk_result>2||(a.flags&~3u)||
     static_cast<unsigned>(a.reason)>13||!geographic(v.latitude_deg,v.longitude_deg,a.seed_msl_m)||!motion::finite(a.start_local))return false;
  // Raw unsuccessful diagnostic hits may contain nonfinite SDK values. They are never surfaces.
  if(a.flags&2u){if(a.sdk_result||!(a.flags&1u)||a.reason!=Reason::none||!motion::finite(a.hit_local)||!motion::finite(a.normal_local)||
      !finite(a.hit_msl_m)||!finite(a.height_residual_m)||!finite(a.horizontal_residual_m)||
      std::abs(a.height_residual_m)>.05||a.horizontal_residual_m<0||a.horizontal_residual_m>.05)return false;}
 }return true;
}
void put(Writer& w,const ProbeAttempt& a) noexcept {
 w.u(static_cast<std::uint64_t>(a.query_start_qpc),8);w.u(static_cast<std::uint64_t>(a.query_end_qpc),8);
 w.u(a.sdk_result,4);w.u(a.flags,4);w.u(static_cast<unsigned>(a.reason),4);w.real(a.seed_msl_m);
 put(w,a.start_local);put(w,a.hit_local);put(w,a.normal_local);w.real(a.hit_msl_m);w.real(a.height_residual_m);w.real(a.horizontal_residual_m);
}
void get(Reader& r,ProbeAttempt& a) noexcept {
 a.query_start_qpc=r.signed64();a.query_end_qpc=r.signed64();a.sdk_result=static_cast<std::uint32_t>(r.u(4));
 a.flags=static_cast<std::uint32_t>(r.u(4));a.reason=static_cast<Reason>(r.u(4));a.seed_msl_m=r.real();
 get(r,a.start_local);get(r,a.hit_local);get(r,a.normal_local);a.hit_msl_m=r.real();a.height_residual_m=r.real();a.horizontal_residual_m=r.real();
}
void put(Writer& w,const ProbeDetail& v) noexcept {
 put(w,v.query);put(w,v.provider);w.u(v.aircraft_generation,8);w.u(v.scenery_generation,8);w.u(v.local_frame_generation,8);
 w.u(v.capture_generation,8);w.u(static_cast<unsigned>(v.purpose),4);w.u(v.count,4);w.u(v.valid_mask,4);w.real(v.latitude_deg);w.real(v.longitude_deg);
 for(const auto& a:v.attempts)put(w,a);put(w,v.geometry_id);w.u(v.geometry_revision,4);w.u(v.assembly_index,4);
}
void get(Reader& r,ProbeDetail& v) noexcept {
 get(r,v.query);get(r,v.provider);v.aircraft_generation=r.u(8);v.scenery_generation=r.u(8);v.local_frame_generation=r.u(8);
 v.capture_generation=r.u(8);v.purpose=static_cast<ProbePurpose>(r.u(4));v.count=static_cast<std::uint32_t>(r.u(4));v.valid_mask=static_cast<std::uint32_t>(r.u(4));v.latitude_deg=r.real();v.longitude_deg=r.real();
 for(auto& a:v.attempts)get(r,a);if(r.version>=3){get(r,v.geometry_id);v.geometry_revision=static_cast<std::uint32_t>(r.u(4));v.assembly_index=static_cast<std::uint32_t>(r.u(4));}
}
bool valid(const SupportDetail& v) noexcept {
 if(!motion::valid(v.producer)||!v.pose_epoch||!v.frame_sequence||!v.capture_generation||v.step_start_qpc<=0||v.step_end_qpc<v.step_start_qpc||
    static_cast<unsigned>(v.native_phase)>3||(v.flags&~3u)||(v.valid_mask&~255u)||v.count>8||
    !finite(v.entry_elev_ft)||!finite(v.shadow_elev_ft)||!finite(v.cached_support_ft)||!finite(v.altitude_damper_ft))return false;
 if((v.flags&2u)&&(!v.terrain_epoch||!v.owner||!v.query_id))return false;
 unsigned used{};for(std::size_t i=0;i<8;++i)if(v.valid_mask&(1u<<i)){++used;const auto& a=v.sites[i];
  if(!geographic(a.latitude_deg,a.longitude_deg,a.plane_ft*.3048)||!finite(a.constraint_ft)||!finite(a.ordinary_strut_ft))return false;
 }return used==v.count;
}
void put(Writer& w,const SupportDetail& v) noexcept {
 put(w,v.producer);w.u(v.pose_epoch,8);w.u(v.terrain_epoch,8);w.u(v.frame_sequence,8);w.u(v.query_id,8);w.u(v.owner,8);w.u(v.capture_generation,8);
 w.u(static_cast<std::uint64_t>(v.step_start_qpc),8);w.u(static_cast<std::uint64_t>(v.step_end_qpc),8);
 w.u(static_cast<unsigned>(v.native_phase),4);w.u(v.flags,4);w.u(v.valid_mask,4);w.u(v.count,4);
 w.real(v.entry_elev_ft);w.real(v.shadow_elev_ft);w.real(v.cached_support_ft);w.real(v.altitude_damper_ft);
 w.u(v.begin_ticks,8);w.u(v.step_ticks,8);w.u(v.helper_ticks,8);
 for(const auto& a:v.sites){w.real(a.latitude_deg);w.real(a.longitude_deg);w.real(a.constraint_ft);w.real(a.plane_ft);w.real(a.ordinary_strut_ft);}
}
void get(Reader& r,SupportDetail& v) noexcept {
 get(r,v.producer);v.pose_epoch=r.u(8);v.terrain_epoch=r.u(8);v.frame_sequence=r.u(8);v.query_id=r.u(8);v.owner=r.u(8);v.capture_generation=r.u(8);
 v.step_start_qpc=r.signed64();v.step_end_qpc=r.signed64();v.native_phase=static_cast<NativePhase>(r.u(4));v.flags=static_cast<std::uint32_t>(r.u(4));v.valid_mask=static_cast<std::uint32_t>(r.u(4));v.count=static_cast<std::uint32_t>(r.u(4));
 v.entry_elev_ft=r.real();v.shadow_elev_ft=r.real();v.cached_support_ft=r.real();v.altitude_damper_ft=r.real();v.begin_ticks=r.u(8);v.step_ticks=r.u(8);v.helper_ticks=r.u(8);
 for(auto& a:v.sites){a.latitude_deg=r.real();a.longitude_deg=r.real();a.constraint_ft=r.real();a.plane_ft=r.real();a.ordinary_strut_ft=r.real();}
}
bool valid(const RenderDetail& v) noexcept {
 if(!valid(v.query)||!motion::valid(v.provider)||!v.capture_generation||v.sample_qpc<=0||(v.valid_mask&~3u))return false;
 if((v.valid_mask&1u)&&(!motion::finite(v.aircraft_local)||!motion::finite(v.aircraft_angles_deg)))return false;
 return !(v.valid_mask&2u)||(motion::finite(v.camera_local)&&motion::finite(v.camera_angles_deg)&&geographic(v.camera_latitude_deg,v.camera_longitude_deg,v.camera_altitude_msl_m));
}
void put(Writer& w,const RenderDetail& v) noexcept {
 put(w,v.query);put(w,v.provider);w.u(v.aircraft_generation,8);w.u(v.scenery_generation,8);w.u(v.local_frame_generation,8);w.u(v.capture_generation,8);
 w.u(static_cast<std::uint64_t>(v.sample_qpc),8);w.u(v.valid_mask,4);put(w,v.aircraft_local);put(w,v.aircraft_angles_deg);put(w,v.camera_local);put(w,v.camera_angles_deg);
 w.real(v.camera_latitude_deg);w.real(v.camera_longitude_deg);w.real(v.camera_altitude_msl_m);
}
void get(Reader& r,RenderDetail& v) noexcept {
 get(r,v.query);get(r,v.provider);v.aircraft_generation=r.u(8);v.scenery_generation=r.u(8);v.local_frame_generation=r.u(8);v.capture_generation=r.u(8);v.sample_qpc=r.signed64();v.valid_mask=static_cast<std::uint32_t>(r.u(4));
 get(r,v.aircraft_local);get(r,v.aircraft_angles_deg);get(r,v.camera_local);get(r,v.camera_angles_deg);v.camera_latitude_deg=r.real();v.camera_longitude_deg=r.real();v.camera_altitude_msl_m=r.real();
}
bool valid(const ConstraintDetail& v) noexcept {
 if(!motion::valid(v.producer)||!v.pose_epoch||!v.frame_sequence||!v.capture_generation||v.step_start_qpc<=0||v.step_end_qpc<v.step_start_qpc||(v.valid_mask&~65535u)||v.count>16)return false;
 unsigned count=0;for(std::size_t i=0;i<16;++i)if(v.valid_mask&(1u<<i)){++count;const auto& a=v.sites[i];if(a.field>1||(a.flags&~1u)||!finite(a.cached_world_rad)||!finite(a.stored_basis_rad)||!finite(a.current_basis_rad)||!finite(a.returned_world_rad))return false;}return count==v.count;
}
void put(Writer& w,const ConstraintDetail& v) noexcept {
 put(w,v.producer);for(auto n:{v.pose_epoch,v.terrain_epoch,v.frame_sequence,v.query_id,v.owner,v.capture_generation})w.u(n,8);
 w.u(static_cast<std::uint64_t>(v.step_start_qpc),8);w.u(static_cast<std::uint64_t>(v.step_end_qpc),8);w.u(v.valid_mask,4);w.u(v.count,4);
 for(const auto& a:v.sites){w.u(a.field,4);w.u(a.flags,4);w.real(a.cached_world_rad);w.real(a.stored_basis_rad);w.real(a.current_basis_rad);w.real(a.returned_world_rad);}
}
void get(Reader& r,ConstraintDetail& v) noexcept {
 get(r,v.producer);v.pose_epoch=r.u(8);v.terrain_epoch=r.u(8);v.frame_sequence=r.u(8);v.query_id=r.u(8);v.owner=r.u(8);v.capture_generation=r.u(8);
 v.step_start_qpc=r.signed64();v.step_end_qpc=r.signed64();v.valid_mask=static_cast<std::uint32_t>(r.u(4));v.count=static_cast<std::uint32_t>(r.u(4));
 for(auto& a:v.sites){a.field=static_cast<std::uint32_t>(r.u(4));a.flags=static_cast<std::uint32_t>(r.u(4));a.cached_world_rad=r.real();a.stored_basis_rad=r.real();a.current_basis_rad=r.real();a.returned_world_rad=r.real();}
}
bool valid(const FrameDetail& v) noexcept {
 if(!motion::valid(v.producer)||!v.pose_epoch||!v.frame_sequence||!v.capture_generation||!v.java_thread_id||!v.os_thread_id||(v.valid_mask&~255u)||(v.flags&~1u)||!finite(v.delta_ms)||!finite(v.delta_acc_ms)||v.delta_ms<0||v.delta_acc_ms<0||(v.valid_mask&129u)!=129u)return false;
 std::int64_t last=0;for(std::size_t i=0;i<8;++i)if(v.valid_mask&(1u<<i)){if(v.stage_qpc[i]<=0||v.stage_qpc[i]<last)return false;last=v.stage_qpc[i];}return true;
}
void put(Writer& w,const FrameDetail& v) noexcept {
 put(w,v.producer);for(auto n:{v.pose_epoch,v.frame_sequence,v.capture_generation,v.java_thread_id})w.u(n,8);w.u(v.valid_mask,4);w.u(v.flags,4);w.u(v.os_thread_id,4);w.real(v.delta_ms);w.real(v.delta_acc_ms);for(auto n:v.stage_qpc)w.u(static_cast<std::uint64_t>(n),8);
}
void get(Reader& r,FrameDetail& v) noexcept {
 get(r,v.producer);v.pose_epoch=r.u(8);v.frame_sequence=r.u(8);v.capture_generation=r.u(8);v.java_thread_id=r.u(8);v.valid_mask=static_cast<std::uint32_t>(r.u(4));v.flags=static_cast<std::uint32_t>(r.u(4));v.os_thread_id=static_cast<std::uint32_t>(r.u(4));v.delta_ms=r.real();v.delta_acc_ms=r.real();for(auto& n:v.stage_qpc)n=r.signed64();
}
template<class T> bool live_valid(const T& v) noexcept {return valid(v);}
bool live_valid(const ProbeDetail& v) noexcept {
 if(!valid(v))return false;
 const bool known=v.geometry_revision==footprint_geometry_revision&&v.geometry_id==sparky_footprint_geometry_id;
 if(v.purpose==ProbePurpose::primary)return known&&v.assembly_index<5;
 return v.assembly_index==no_assembly&&(known||(!v.geometry_revision&&!motion::valid(v.geometry_id)));
}
template<class T> bool pack(const T& v,Kind kind,std::uint32_t sequence,Frame& out) noexcept {
 if(!live_valid(v))return false;Frame temp;Writer w{temp.bytes.data()+header_size,max_payload};put(w,v);if(!w.ok)return false;
 Writer h{temp.bytes.data(),header_size};h.u(0x33545850,4);h.u(protocol_version,2);h.u(static_cast<unsigned>(kind),2);h.u(w.pos,4);h.u(sequence,4);
 temp.size=header_size+w.pos;out=temp;return true;
}
template<class T> bool unpack(const std::uint8_t* data,std::size_t size,T& out) noexcept {if(size>max_payload)return false;T temp;Reader r{data,size};get(r,temp);if(!r.end()||!live_valid(temp))return false;out=temp;return true;}
} // namespace
bool valid(const Reference& v) noexcept {
 return motion::valid(v.producer)&&v.pose_epoch&&v.sequence&&v.frame_sequence&&v.sample_qpc>0&&v.received_qpc>=v.sample_qpc-1&&
 geographic(v.latitude_deg,v.longitude_deg,v.altitude_msl_m)&&finite(v.heading_rad)&&finite(v.pitch_rad)&&finite(v.bank_left_rad)&&finite(v.main_strut_ft)&&v.reference_revision==motion::instant_reference_revision&&
 std::abs(v.heading_rad)<=7&&std::abs(v.pitch_rad)<=2&&std::abs(v.bank_left_rad)<=4&&(v.flags&1)&&(v.flags&(4|8|128|256))==0;
}
bool valid(const Surface& v) noexcept {return valid(v.query)&&motion::valid(v.provider)&&v.query_start_qpc>0&&v.query_end_qpc>=v.query_start_qpc&&
 geographic(v.latitude_deg,v.longitude_deg,v.height_msl_m)&&(v.flags&~31u)==0&&normal(v.normal_east,v.normal_north,v.normal_up);}
bool reference_eye(const Reference& v,motion::GeoPose& out) noexcept {
 if(!valid(v))return false;motion::RawPose r;r.sequence=v.sequence;r.frame_sequence=v.frame_sequence;r.pose_epoch=v.pose_epoch;r.sample_start_qpc=r.sample_end_qpc=v.sample_qpc;r.flags=v.flags;
 r.latitude_rad=v.latitude_deg*motion::pi/180;r.longitude_rad=v.longitude_deg*motion::pi/180;r.heading_rad=v.heading_rad;r.total_pitch_rad=static_cast<float>(v.pitch_rad);r.bank_rad=static_cast<float>(v.bank_left_rad);r.altitude_ft=static_cast<float>(v.altitude_msl_m/.3048);r.main_strut_ft=static_cast<float>(v.main_strut_ft);
 motion::CompletePoseSample pose;return motion::derive_pose(r,v.received_qpc,1,pose,v.reference_revision)&&motion::geographic_pose(pose,out);
}
bool geographic_offset(double alat,double alon,double lat,double lon,GeographicOffset& out) noexcept {
 if(!geographic(alat,alon,0)||!geographic(lat,lon,0))return false;
 GeographicOffset v;v.east_m=std::remainder((lon-alon)*motion::pi/180,2*motion::pi)*6378137*std::cos(alat*motion::pi/180);v.north_m=(lat-alat)*motion::pi/180*6378137;
 if(!finite(v.east_m)||!finite(v.north_m))return false;out=v;return true;
}
bool fit_footprint(Surface& v) noexcept {
 if(v.station_mask!=31)return false;const auto& a=v.stations[0];
 double le=(v.stations[2].east_m+v.stations[4].east_m)*.5,ln=(v.stations[2].north_m+v.stations[4].north_m)*.5,lh=(v.stations[2].height_msl_m+v.stations[4].height_msl_m)*.5;
 double re=(v.stations[1].east_m+v.stations[3].east_m)*.5,rn=(v.stations[1].north_m+v.stations[3].north_m)*.5,rh=(v.stations[1].height_msl_m+v.stations[3].height_msl_m)*.5;
 le-=a.east_m;ln-=a.north_m;lh-=a.height_msl_m;re-=a.east_m;rn-=a.north_m;rh-=a.height_msl_m;
 const double determinant=le*rn-re*ln;if(!finite(determinant)||std::abs(determinant)<1e-6)return false;
 const double east=(lh*rn-rh*ln)/determinant,north=(le*rh-re*lh)/determinant,up=std::sqrt(1+east*east+north*north);
 const double h=a.height_msl_m-east*a.east_m-north*a.north_m;if(!geographic(v.query.reference.latitude_deg,v.query.reference.longitude_deg,h)||!finite(up)||up<=0)return false;
 v.latitude_deg=v.query.reference.latitude_deg;v.longitude_deg=v.query.reference.longitude_deg;v.height_msl_m=h;v.normal_east=-east/up;v.normal_north=-north/up;v.normal_up=1/up;
 for(auto& x:v.stations){x.plane_residual_m=x.height_msl_m-(h+east*x.east_m+north*x.north_m);if(!finite(x.plane_residual_m))return false;}return true;
}
bool qualified_footprint(const Surface& v) noexcept {
 if(!valid(v)||v.geometry_revision!=footprint_geometry_revision||v.geometry_id!=sparky_footprint_geometry_id||v.station_mask!=31||!v.aircraft_generation||!v.scenery_generation||!v.local_frame_generation||
 (v.flags&(hit|geometry_qualified|footprint_plane))!=(hit|geometry_qualified|footprint_plane)||(v.flags&(wet|moving)))return false;
 motion::GeoPose eye;if(!reference_eye(v.query.reference,eye))return false;
 for(const auto& a:v.stations){
  if(!geographic(a.latitude_deg,a.longitude_deg,a.height_msl_m)||!finite(a.east_m)||!finite(a.north_m)||std::abs(a.east_m)>1000||std::abs(a.north_m)>1000||!normal(a.normal_east,a.normal_north,a.normal_up)||!finite(a.plane_residual_m)||a.query_start_qpc<v.query_start_qpc||a.query_end_qpc<a.query_start_qpc||a.query_end_qpc>v.query_end_qpc)return false;
  GeographicOffset offset;if(!geographic_offset(v.query.reference.latitude_deg,v.query.reference.longitude_deg,a.latitude_deg,a.longitude_deg,offset)||std::abs(offset.east_m-a.east_m)>1e-5||std::abs(offset.north_m-a.north_m)>1e-5)return false;
 }
 Surface expected=v;if(!fit_footprint(expected)||std::abs(expected.height_msl_m-v.height_msl_m)>1e-5||std::abs(expected.latitude_deg-v.latitude_deg)>1e-10||std::abs(expected.longitude_deg-v.longitude_deg)>1e-10||std::abs(expected.normal_east-v.normal_east)>1e-8||std::abs(expected.normal_north-v.normal_north)>1e-8||std::abs(expected.normal_up-v.normal_up)>1e-8)return false;
 for(std::size_t i=0;i<5;++i)if(std::abs(expected.stations[i].plane_residual_m-v.stations[i].plane_residual_m)>1e-5)return false;return true;
}
namespace {
template<class T> bool capture_payload(const std::uint8_t* data,std::size_t size,std::uint16_t version) noexcept {
 T v;Reader r{data,size,0,true,version};get(r,v);return r.end()&&(version==3?live_valid(v):valid(v));
}
}
bool validate_capture_payload(const Header& h,const std::uint8_t* data,std::size_t size,std::uint16_t version) noexcept {
 if(version<1||version>3||static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>(version==1?8u:version==2?12u:14u)||size!=h.payload_size||size>max_payload)return false;
 #define CASE(T,K) case Kind::K:return capture_payload<T>(data,size,version)
 switch(h.kind){CASE(Hello,hello);CASE(Subscribe,subscribe);CASE(Query,query);CASE(Surface,surface);CASE(Control,control);CASE(Ack,ack);CASE(Consumed,consumed);CASE(Diagnostics,diagnostics);CASE(ProbeDetail,probe_detail);CASE(SupportDetail,support_detail);CASE(RenderDetail,render_detail);CASE(Capture,capture);CASE(ConstraintDetail,constraint_detail);CASE(FrameDetail,frame_detail);default:return false;}
 #undef CASE
}
bool read_header(const std::uint8_t* d,std::size_t n,Header& out) noexcept {
 Header candidate;std::uint16_t version{};
 if(!read_capture_header(d,n,candidate,version)||version!=protocol_version)return false;
 out=candidate;return true;
}
bool read_capture_header(const std::uint8_t* d,std::size_t n,Header& out,std::uint16_t& version) noexcept {
 if(n!=header_size)return false;Reader r{d,n};const auto magic=r.u(4),v=r.u(2);
 if(!((magic==0x31545850&&v==1)||(magic==0x32545850&&v==2)||(magic==0x33545850&&v==3)))return false;
 Header h;h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));
 if(!r.end()||static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>(v==1?8u:v==2?12u:14u)||h.payload_size>max_payload)return false;out=h;version=static_cast<std::uint16_t>(v);return true;
}
bool encode(const Hello& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::hello,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Hello& v) noexcept {return unpack(d,n,v);}
bool encode(const Subscribe& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::subscribe,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Subscribe& v) noexcept {return unpack(d,n,v);}
bool encode(const Query& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::query,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Query& v) noexcept {return unpack(d,n,v);}
bool encode(const Surface& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::surface,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Surface& v) noexcept {return unpack(d,n,v);}
bool encode(const Control& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::control,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Control& v) noexcept {return unpack(d,n,v);}
bool encode(const Ack& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::ack,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Ack& v) noexcept {return unpack(d,n,v);}
bool encode(const Consumed& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::consumed,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Consumed& v) noexcept {return unpack(d,n,v);}
bool encode(const Diagnostics& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::diagnostics,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Diagnostics& v) noexcept {return unpack(d,n,v);}
#define DETAIL_CODEC(T,K) bool encode(const T& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::K,n,f);} bool decode(const std::uint8_t* d,std::size_t n,T& v) noexcept {return unpack(d,n,v);}
DETAIL_CODEC(ProbeDetail,probe_detail)
DETAIL_CODEC(SupportDetail,support_detail)
DETAIL_CODEC(RenderDetail,render_detail)
DETAIL_CODEC(Capture,capture)
DETAIL_CODEC(ConstraintDetail,constraint_detail)
DETAIL_CODEC(FrameDetail,frame_detail)
#undef DETAIL_CODEC
bool Framer::feed(const void* data,std::size_t size,FrameHandler handler,void* context) noexcept {
 if(failed_||(!data&&size)||!handler){failed_=true;return false;}auto p=static_cast<const std::uint8_t*>(data);
 while(size){const auto amount=std::min(size,needed_-used_);std::memcpy(bytes_.data()+used_,p,amount);used_+=amount;p+=amount;size-=amount;
  if(used_!=needed_)continue;Header h;if(!read_header(bytes_.data(),header_size,h)){failed_=true;return false;}
  if(needed_==header_size&&h.payload_size){needed_=header_size+h.payload_size;continue;}
  if(!handler(context,h,bytes_.data()+header_size,h.payload_size)){failed_=true;return false;}used_=0;needed_=header_size;
 }return true;
}
} // namespace psx::terrain
