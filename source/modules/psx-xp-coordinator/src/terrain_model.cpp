#include "coordinator/terrain.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace coordinator {
namespace t=psx::terrain;namespace m=psx::motion;
namespace {
std::size_t lane(t::Role role){return role==t::Role::exporter?1:0;}
bool equal(const t::Query& a,const t::Query& b){t::Frame x,y;return t::encode(a,0,x)&&t::encode(b,0,y)&&x.size==y.size&&!std::memcmp(x.bytes.data(),y.bytes.data(),x.size);}
bool equal(const t::Control& a,const t::Control& b){t::Frame x,y;return t::encode(a,0,x)&&t::encode(b,0,y)&&x.size==y.size&&!std::memcmp(x.bytes.data(),y.bytes.data(),x.size);}
bool physical_capable(const t::Hello& hello) noexcept {const auto required=hello.role==t::Role::exporter?t::native_support:(t::surface_query|t::footprint_surface);return (hello.capabilities&required)==required;}
}
const char* terrain_phase_name(t::Phase v) noexcept {constexpr const char* names[]={"Unavailable","Native","Blending","Active","Held"};const auto i=static_cast<unsigned>(v);return i<std::size(names)?names[i]:"Unknown";}
const char* terrain_reason_name(t::Reason v) noexcept {constexpr const char* names[]={"None","Unsupported","Identity","Epoch","Stale","Invalid surface","Owner","Expired","Stopped","Disconnected","Overflow","Scenery","Local frame","Loading"};const auto i=static_cast<unsigned>(v);return i<std::size(names)?names[i]:"Unknown";}
void TerrainModel::configure(std::uint64_t frequency,std::uint64_t owner) noexcept {frequency_=frequency;view_.owner=owner;}
void TerrainModel::capture(bool enabled,std::int64_t now) noexcept {if(enabled==capture_enabled_)return;capture_enabled_=enabled;++capture_generation_;capture_started_qpc_=enabled?now:0;capture_peer_started_qpc_.fill(0);capture_ready_.fill(true);}
bool TerrainModel::capture_detail(const TerrainEvent& event) const noexcept {
    if(!capture_enabled_||event.kind!=TerrainEventKind::frame)return false;
    const auto& peer=event.role==t::Role::exporter?view_.exporter:view_.plugin;
    const auto i=event.role==t::Role::exporter?1u:0u;
    const auto floor=(std::max)(capture_started_qpc_,capture_peer_started_qpc_[i]);
    if(!peer.connected||!capture_peer_started_qpc_[i]||event.connection_generation!=peer.connection_generation||event.process.pid!=peer.hello.pid||event.process.creation_filetime!=peer.hello.process_creation_filetime)return false;
    t::Header h;if(event.frame.size<t::header_size||!t::read_header(event.frame.bytes.data(),t::header_size,h)||event.frame.size!=t::header_size+h.payload_size)return false;
    const auto* bytes=event.frame.bytes.data()+t::header_size;
    if(event.role==t::Role::plugin&&view_.plugin.connected){
        if(h.kind==t::Kind::probe_detail){t::ProbeDetail d;if(!(peer.hello.capabilities&t::probe_details))return false;
            if(!t::decode(bytes,h.payload_size,d)||d.provider!=view_.plugin.hello.instance||d.capture_generation!=capture_generation_||d.query.owner!=view_.owner||d.query.reference.producer!=view_.reference.producer||d.query.reference.pose_epoch!=view_.reference.pose_epoch)return false;
            for(std::size_t n=0;n<d.count;++n)if(d.attempts[n].query_start_qpc<floor||d.attempts[n].query_end_qpc>event.received_qpc)return false;return true;
        }
        if(h.kind==t::Kind::render_detail){t::RenderDetail d;return (peer.hello.capabilities&t::render_details)&&t::decode(bytes,h.payload_size,d)&&d.provider==view_.plugin.hello.instance&&d.capture_generation==capture_generation_&&d.query.owner==view_.owner&&d.query.reference.producer==view_.reference.producer&&d.query.reference.pose_epoch==view_.reference.pose_epoch&&d.sample_qpc>=floor&&d.sample_qpc<=event.received_qpc;}
    }
    if(event.role==t::Role::exporter&&view_.exporter.connected&&h.kind==t::Kind::support_detail){t::SupportDetail d;
        return (peer.hello.capabilities&t::support_details)&&t::decode(bytes,h.payload_size,d)&&d.producer==view_.exporter.hello.instance&&d.producer==view_.reference.producer&&d.pose_epoch==view_.reference.pose_epoch&&d.capture_generation==capture_generation_&&d.step_start_qpc>=floor&&d.step_end_qpc<=event.received_qpc;
    }
    if(event.role==t::Role::exporter&&h.kind==t::Kind::constraint_detail){t::ConstraintDetail d;
        return (peer.hello.capabilities&t::constraint_details)&&t::decode(bytes,h.payload_size,d)&&d.producer==peer.hello.instance&&d.producer==view_.reference.producer&&d.pose_epoch==view_.reference.pose_epoch&&d.capture_generation==capture_generation_&&d.step_start_qpc>=floor&&d.step_end_qpc<=event.received_qpc;
    }
    if(event.role==t::Role::exporter&&h.kind==t::Kind::frame_detail){t::FrameDetail d;
        if(!(peer.hello.capabilities&t::frame_details)||!t::decode(bytes,h.payload_size,d)||d.producer!=peer.hello.instance||d.producer!=view_.reference.producer||d.pose_epoch!=view_.reference.pose_epoch||d.capture_generation!=capture_generation_||!(d.valid_mask&1u)||d.stage_qpc[0]<floor)return false;
        for(std::size_t n=0;n<d.stage_qpc.size();++n)if((d.valid_mask&(1u<<n))&&d.stage_qpc[n]>event.received_qpc)return false;return true;
    }
    return false;
}
bool TerrainModel::fresh(std::int64_t now,std::int64_t time,std::uint32_t ms) const noexcept {return frequency_&&time>=0&&now>=time&&now-time<static_cast<std::int64_t>(frequency_*ms/1000);}
bool TerrainModel::control(std::size_t i,t::Action action,std::int64_t now) noexcept {
    auto& p=peer(i);auto& l=lanes_[i];if(!p.connected)return false;
    if(l.size==l.controls.size()){++view_.overflows;l.blocked=true;p.owned=p.pending=false;view_.reason=t::Reason::overflow;return false;}
    t::Control c;c.producer=view_.reference.producer;c.owner=view_.owner;c.operation=++operation_;c.terrain_epoch=view_.terrain_epoch;c.pose_epoch=view_.reference.pose_epoch;c.action=action;
    t::Frame checked;if(!t::encode(c,0,checked))return false;
    l.controls[(l.head+l.size)%l.controls.size()]={c,now};++l.size;++view_.controls;
    if(action!=t::Action::release)p.pending=true;l.renewed_qpc=now;return true;
}
void TerrainModel::release(t::Reason reason,std::int64_t now) noexcept {
    for(std::size_t i=0;i<2;++i){auto& p=peer(i);auto& l=lanes_[i];
        const bool had=p.owned||p.pending;l.head=l.size=l.waiting=0;
        if(had)control(i,t::Action::release,now);p.owned=p.pending=false;p.expires_qpc=0;
    }
    query_ready_=surface_ready_=false;for(auto& p:pending_)p.active=false;
    view_.phase=view_.consumed_available?t::Phase::held:t::Phase::native;view_.reason=reason;
}
void TerrainModel::boundary(t::Reason reason,std::int64_t now) noexcept {release(reason,now);++view_.terrain_epoch;++view_.boundaries;view_.surface_available=view_.diagnostics_available=false;}
void TerrainModel::physical(const PhysicalPoseCache& cache,std::int64_t now) noexcept {
    if(!cache.available){if(view_.physical_available)boundary(t::Reason::disconnected,now);view_.physical_available=view_.physical_fresh=false;return;}
    if(cache.revision==view_.physical_revision&&cache.producer==view_.reference.producer)return;
    const auto& r=cache.raw;if(!valid_physical_pose(r,cache.received_qpc)){++view_.rejected;return;}
    if(view_.physical_available){
        if(cache.producer==view_.reference.producer&&(r.pose_epoch<view_.reference.pose_epoch||r.sequence<=view_.reference.sequence||r.frame_sequence<=view_.reference.frame_sequence||r.sample_end_qpc<view_.reference.sample_qpc)){++view_.rejected;return;}
        if(cache.producer!=view_.reference.producer||r.pose_epoch!=view_.reference.pose_epoch||
           r.sample_end_qpc-view_.reference.sample_qpc>=static_cast<std::int64_t>(frequency_*t::stale_ms/1000)||
           (r.flags&(m::raw_ordinary_altitude|m::raw_paused_altitude))!=(view_.reference.flags&(m::raw_ordinary_altitude|m::raw_paused_altitude)))boundary(t::Reason::epoch,now);
    }
    t::Reference ref;ref.producer=cache.producer;ref.pose_epoch=r.pose_epoch;ref.sequence=r.sequence;ref.frame_sequence=r.frame_sequence;
    ref.sample_qpc=r.sample_end_qpc;ref.received_qpc=cache.received_qpc;ref.latitude_deg=r.latitude_rad*180/m::pi;ref.longitude_deg=r.longitude_rad*180/m::pi;
    ref.altitude_msl_m=static_cast<double>(r.altitude_ft)*0.3048;ref.heading_rad=r.heading_rad;ref.pitch_rad=r.total_pitch_rad;ref.bank_left_rad=r.bank_rad;ref.flags=r.flags;
    ref.main_strut_ft=r.main_strut_ft;ref.reference_revision=m::instant_reference_revision;
    if(!t::valid(ref)){++view_.rejected;return;}view_.reference=ref;view_.physical_available=true;view_.physical_revision=cache.revision;
    if(view_.consumed_available&&(view_.consumed.producer!=ref.producer||view_.consumed.pose_epoch!=ref.pose_epoch))view_.consumed_available=false;
}
void TerrainModel::following(bool value,std::int64_t now) noexcept {
    if(view_.following==value)return;view_.following=value;
    if(!value)release(t::Reason::stopped,now);else {++view_.terrain_epoch;for(auto& l:lanes_)l.blocked=false;view_.reason=t::Reason::none;}
}
void TerrainModel::lost(std::size_t i,std::uint32_t error,std::int64_t now) noexcept {
    const auto retired=peer(i).connection_generation;
    boundary(t::Reason::disconnected,now);peer(i)={};peer(i).error=error;peer(i).connection_generation=retired;lanes_[i]={};
}
bool TerrainModel::event(const TerrainEvent& e) noexcept {
    if(e.received_qpc<0||static_cast<unsigned>(e.role)<1||static_cast<unsigned>(e.role)>2)return false;
    const auto i=lane(e.role);auto& p=peer(i);auto& l=lanes_[i];
    // A worker can finish an old read while its loss/replacement is queued.
    // Keep the retired scope as a high-water mark: that evidence cannot retire
    // a newer service, or turn its own already-retired frames into new faults.
    if(e.connection_generation<p.connection_generation)return true;
    if(e.kind==TerrainEventKind::lost){
        if(e.connection_generation==p.connection_generation&&!p.connected)return true;
        if(e.connection_generation==p.connection_generation&&p.connected&&e.process.pid&&
           (e.process.pid!=p.hello.pid||e.process.creation_filetime!=p.hello.process_creation_filetime))return false;
        lost(i,e.error,e.received_qpc);p.connection_generation=e.connection_generation;return true;
    }
    if(e.connection_generation&&e.connection_generation==p.connection_generation&&!p.connected)return true;
    t::Header h;if(e.frame.size<t::header_size||!t::read_header(e.frame.bytes.data(),t::header_size,h)||e.frame.size!=t::header_size+h.payload_size)return false;
    const auto* bytes=e.frame.bytes.data()+t::header_size;
    if(h.kind==t::Kind::hello){t::Hello hello;if(!t::decode(bytes,h.payload_size,hello)||hello.role!=e.role||hello.pid!=e.process.pid||hello.process_creation_filetime!=e.process.creation_filetime||hello.qpc_frequency!=frequency_)return false;
        if(e.connection_generation&&e.connection_generation==p.connection_generation)return false;
        if(p.connected)boundary(t::Reason::identity,e.received_qpc);if(i==0)generations_known_=false;p={};p.hello=hello;p.connected=true;l={};l.blocked=!physical_capable(hello);
        p.connection_generation=e.connection_generation;capture_peer_started_qpc_[i]=0;capture_ready_[i]=true;
        view_.reason=(view_.plugin.connected&&!physical_capable(view_.plugin.hello))||(view_.exporter.connected&&!physical_capable(view_.exporter.hello))?t::Reason::unsupported:t::Reason::none;return true;
    }
    if(!p.connected||e.connection_generation!=p.connection_generation||e.process.pid!=p.hello.pid||e.process.creation_filetime!=p.hello.process_creation_filetime)return false;
    // Details are capture evidence only. They never update authority/status or
    // enter the automatic raw journal, even with unsupported native support.
    if(i==0&&h.kind==t::Kind::probe_detail){t::ProbeDetail d;return t::decode(bytes,h.payload_size,d);}
    if(i==0&&h.kind==t::Kind::render_detail){t::RenderDetail d;return t::decode(bytes,h.payload_size,d);}
    if(i==1&&h.kind==t::Kind::support_detail){t::SupportDetail d;return t::decode(bytes,h.payload_size,d);}
    if(i==1&&h.kind==t::Kind::constraint_detail){t::ConstraintDetail d;return t::decode(bytes,h.payload_size,d);}
    if(i==1&&h.kind==t::Kind::frame_detail){t::FrameDetail d;return t::decode(bytes,h.payload_size,d);}
    if(h.kind==t::Kind::ack){t::Ack ack;if(!t::decode(bytes,h.payload_size,ack))return false;
        std::size_t found=l.waiting;for(std::size_t n=0;n<l.waiting;++n)if(equal(ack.control,l.awaiting[n].value)){found=n;break;}
        if(found==l.waiting){++view_.rejected;return true;}
        const auto entry=l.awaiting[found];for(std::size_t n=found+1;n<l.waiting;++n)l.awaiting[n-1]=l.awaiting[n];--l.waiting;
        if(ack.control.action==t::Action::release){++view_.acks;return true;}
        if(ack.control.terrain_epoch!=view_.terrain_epoch||ack.control.producer!=view_.reference.producer||ack.control.pose_epoch!=view_.reference.pose_epoch||!view_.following||!fresh(e.received_qpc,entry.sent_qpc,t::lease_ms)){++view_.rejected;return true;}
        ++view_.acks;p.ack_qpc=e.received_qpc;p.pending=false;
        if(ack.reason!=t::Reason::none){boundary(ack.reason,e.received_qpc);l.blocked=true;l.blocked_reason=ack.reason;l.blocked_at=e.received_qpc;return true;}
        p.owned=true;p.expires_qpc=entry.sent_qpc+static_cast<std::int64_t>(frequency_*ack.control.duration_ms/1000);return true;
    }
    if(h.kind==t::Kind::surface&&i==0){t::Surface s;if(!t::decode(bytes,h.payload_size,s))return false;
        Pending* match=nullptr;for(auto& q:pending_)if(q.active&&equal(q.query,s.query)){match=&q;break;}
        if(!match||!p.owned||!view_.following||s.provider!=p.hello.instance||s.query.terrain_epoch!=view_.terrain_epoch||s.query.reference.producer!=view_.reference.producer||s.query.reference.pose_epoch!=view_.reference.pose_epoch||
           !fresh(e.received_qpc,s.query.reference.sample_qpc,t::stale_ms)||!fresh(e.received_qpc,s.query_end_qpc,t::stale_ms)||s.query_start_qpc<s.query.reference.received_qpc||s.query_end_qpc>e.received_qpc||
           !(s.flags&t::hit)||(s.flags&(t::wet|t::moving))||!t::valid(s)||!t::qualified_footprint(s)){++view_.rejected;view_.reason=t::Reason::invalid_surface;return true;}
        match->active=false;
        if(generations_known_&&(s.aircraft_generation!=aircraft_generation_||s.scenery_generation!=scenery_generation_||s.local_frame_generation!=local_generation_)){
            if(s.aircraft_generation<aircraft_generation_||s.scenery_generation<scenery_generation_||s.local_frame_generation<local_generation_){++view_.rejected;return true;}
            const auto reason=s.local_frame_generation!=local_generation_?t::Reason::local_frame:t::Reason::scenery;
            aircraft_generation_=s.aircraft_generation;scenery_generation_=s.scenery_generation;local_generation_=s.local_frame_generation;
            boundary(reason,e.received_qpc);view_.surface=s;return true;
        }
        if(view_.surface_available&&s.query.query_id<=view_.surface.query.query_id){++view_.rejected;return true;}
        view_.surface=s;view_.surface_available=true;view_.surface_received_qpc=e.received_qpc;++view_.surfaces;view_.reason=t::Reason::none;
        generations_known_=true;aircraft_generation_=s.aircraft_generation;scenery_generation_=s.scenery_generation;local_generation_=s.local_frame_generation;
        surface_=s;surface_ready_=true;return true;
    }
    if(h.kind==t::Kind::consumed&&i==1){t::Consumed c;if(!t::decode(bytes,h.payload_size,c))return false;
        if(c.producer!=p.hello.instance||c.producer!=view_.reference.producer||c.pose_epoch!=view_.reference.pose_epoch||c.consumed_qpc>e.received_qpc||c.source_sample_qpc<0||c.surface_qpc<0||c.source_sample_qpc>c.consumed_qpc||c.surface_qpc>c.consumed_qpc||
           (view_.consumed_available&&c.pose_epoch==view_.consumed.pose_epoch&&c.consumed_qpc<view_.consumed.consumed_qpc)){++view_.rejected;return true;}
        view_.consumed=c;view_.consumed_available=true;view_.consumed_received_qpc=e.received_qpc;view_.phase=c.phase;return true;
    }
    if(h.kind==t::Kind::diagnostics&&i==0){t::Diagnostics d;if(!t::decode(bytes,h.payload_size,d))return false;
        if(d.provider!=p.hello.instance||d.query.terrain_epoch!=view_.terrain_epoch||!view_.surface_available||!equal(d.query,view_.surface.query)||d.aircraft_generation!=view_.surface.aircraft_generation||d.scenery_generation!=view_.surface.scenery_generation||d.local_frame_generation!=view_.surface.local_frame_generation){++view_.rejected;return true;}
        view_.diagnostics=d;view_.diagnostics_available=true;return true;
    }
    return false;
}
void TerrainModel::tick(std::int64_t now) noexcept {
    const bool current=view_.physical_available&&fresh(now,view_.reference.sample_qpc,t::stale_ms)&&fresh(now,view_.reference.received_qpc,t::stale_ms);
    if(view_.physical_fresh&&!current)boundary(t::Reason::stale,now);view_.physical_fresh=current;
    for(auto& q:pending_)if(q.active&&!fresh(now,q.query.reference.sample_qpc,t::stale_ms))q.active=false;
    if(!view_.following||!current)return;
    if((view_.plugin.connected&&!physical_capable(view_.plugin.hello))||(view_.exporter.connected&&!physical_capable(view_.exporter.hello))){release(t::Reason::unsupported,now);return;}
    if(view_.exporter.connected&&view_.exporter.hello.instance!=view_.reference.producer){release(t::Reason::identity,now);return;}
    for(std::size_t i=0;i<2;++i){auto& p=peer(i);auto& l=lanes_[i];if(!p.connected||!physical_capable(p.hello))continue;
        if(l.blocked&&(l.blocked_reason==t::Reason::loading||l.blocked_reason==t::Reason::epoch||l.blocked_reason==t::Reason::stale||l.blocked_reason==t::Reason::scenery||l.blocked_reason==t::Reason::local_frame)&&view_.reference.sample_qpc>l.blocked_at&&now-l.blocked_at>=static_cast<std::int64_t>(frequency_*t::renew_ms/1000))l.blocked=false;
        if(l.blocked)continue;
        if(p.owned&&now>=p.expires_qpc){release(t::Reason::expired,now);l.blocked=true;return;}
        if(p.pending&&l.waiting&&now-l.awaiting[0].sent_qpc>=static_cast<std::int64_t>(frequency_*t::lease_ms/1000)){release(t::Reason::expired,now);l.blocked=true;return;}
        if(!p.owned&&!p.pending)control(i,t::Action::acquire,now);
        else if(p.owned&&!p.pending&&now-l.renewed_qpc>=static_cast<std::int64_t>(frequency_*t::renew_ms/1000))control(i,t::Action::renew,now);
    }
    if(view_.plugin.owned&&(!last_query_qpc_||now-last_query_qpc_>=static_cast<std::int64_t>(frequency_/100))){
        query_={view_.owner,view_.terrain_epoch,++query_id_,view_.reference};query_ready_=true;last_query_qpc_=now;
    }
}
bool TerrainModel::next(t::Role role,t::Frame& frame,std::int64_t now) noexcept {
    const auto i=lane(role);auto& l=lanes_[i];auto& p=peer(i);
    if(l.size){const auto value=l.controls[l.head];l.head=(l.head+1)%l.controls.size();--l.size;
        if(l.waiting==l.awaiting.size()){++view_.overflows;l.blocked=true;return false;}
        l.awaiting[l.waiting++]={value.value,now};return t::encode(value.value,0,frame);
    }
    const auto detail_capabilities=i?(t::support_details|t::constraint_details|t::frame_details):(t::probe_details|t::render_details);
    if(p.connected&&capture_ready_[i]){capture_ready_[i]=false;if(p.hello.capabilities&detail_capabilities){t::Capture c;c.owner=view_.owner;c.generation=capture_generation_;c.enabled=capture_enabled_?1u:0u;capture_peer_started_qpc_[i]=capture_enabled_?now:0;return t::encode(c,0,frame);}}
    if(!p.owned||!view_.following||!view_.physical_fresh)return false;
    if(i==0&&query_ready_){query_ready_=false;Pending* free=nullptr;for(auto& q:pending_)if(!q.active){free=&q;break;}
        if(!free){++view_.overflows;view_.reason=t::Reason::overflow;return false;}*free={query_,true};++view_.queries;return t::encode(query_,0,frame);
    }
    if(i==1&&surface_ready_){surface_ready_=false;if(!view_.plugin.owned||!fresh(now,surface_.query.reference.sample_qpc,t::stale_ms)||!fresh(now,surface_.query_end_qpc,t::stale_ms))return false;return t::encode(surface_,0,frame);}
    return false;
}
}
