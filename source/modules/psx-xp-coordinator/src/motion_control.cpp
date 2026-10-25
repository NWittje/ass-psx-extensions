#include "coordinator/motion_control.hpp"
#include <algorithm>
namespace coordinator {
namespace b=psx::bridge;namespace m=psx::motion;
const char* motion_phase_name(m::Phase value) noexcept {constexpr const char* names[]={"Observing","Waiting","Relocating","Following","Holding","Stopped","Fault"};const auto i=static_cast<unsigned>(value);return i<7?names[i]:"Unknown";}
const char* motion_reason_name(m::Reason value) noexcept {constexpr const char* names[]={"None","Source unavailable","Boost delayed","Invalid pose","Polar/grid heading unavailable","Epoch changed","Aircraft profile unqualified","Binding unavailable","Override already owned","Override lost","Competing writer","Pause failed","Relocation timeout","Relocation failed","Aircraft changed","Stale request","Cancelled","Disconnected","Historical PSX pause/freeze hold","Scenery loading","Reference unqualified","Control overflow","Disabled"};const auto i=static_cast<unsigned>(value);return i<23?names[i]:"Unknown";}
bool MotionControl::record(MotionRecordKind kind,const b::Frame& frame,std::int64_t time,bool local,bool reserved) {
    const auto limit=reserved?record_capacity:record_capacity-operation_capacity;
    if(record_size_>=limit){++view_.control_overflows;return false;}
    records_[(record_head_+record_size_)%records_.size()]={kind,local,time,frame};++record_size_;return true;
}
bool MotionControl::next_record(MotionRecord& out){if(!record_size_)return false;out=records_[record_head_];record_head_=(record_head_+1)%records_.size();--record_size_;return true;}
void MotionControl::remember(const b::MotionRequest& request){retired_[retired_next_++%retired_.size()]=request;}
void MotionControl::cancel(std::int64_t now,m::Reason reason) {
    for(auto& p:pending_)if(p.active){b::MotionAck ack;ack.connection_id=p.request.connection_id;ack.operation_id=p.request.operation_id;ack.plugin_load_id=p.request.plugin_load_id;ack.enable_id=p.request.enable_id;ack.aircraft_generation=p.request.aircraft_generation;ack.generated_qpc=now;ack.result=m::AckResult::cancelled;ack.reason=reason;b::Frame frame;
        if(b::encode(ack,0,frame))record(MotionRecordKind::ack,frame,now,true,true);view_.last_ack=ack;view_.last_ack_local=true;++view_.cancelled;remember(p.request);p={};}
    queue_head_=queue_size_=0;view_.queued=view_.outstanding=0;
}
void MotionControl::fault(std::int64_t now){if(view_.transport_fault)return;view_.transport_fault=true;cancel(now,m::Reason::transport_overflow);}
void MotionControl::attach(std::uint64_t connection,std::int64_t now) {
    if(view_.outstanding)cancel(now,m::Reason::disconnected);view_.connected=true;view_.connection_id=connection;view_.identity_available=false;view_.status_available=false;view_.transport_fault=false;view_.status_received_qpc=0;load_=enable_=aircraft_=0;
}
void MotionControl::detach(std::int64_t now,m::Reason reason){cancel(now,reason);view_.connected=false;view_.identity_available=false;view_.status_available=false;}
bool MotionControl::identities(std::uint64_t load,std::uint64_t enable,std::uint64_t aircraft,std::int64_t now) {
    // PluginStatus and MotionStatus have independent snapshot handoffs. An older
    // callback snapshot may arrive later; never retarget requests backwards.
    if(load_==load && enable_==enable && aircraft<aircraft_)return false;
    // Placement may reinitialize the SAME aircraft. Its definitive completion
    // still echoes the request's old aircraft generation. Only a changed plugin
    // lifetime makes that pending outcome ambiguous here; the plugin settles
    // unrelated aircraft replacements through explicit terminal acks.
    if(load_ && (load_!=load || enable_!=enable))cancel(now,m::Reason::disabled);
    if(load_!=load || enable_!=enable || aircraft_!=aircraft)view_.status_available=false;
    load_=load;enable_=enable;aircraft_=aircraft;view_.identity_available=load && enable;return true;
}
void MotionControl::plugin(const b::PluginStatus& p,std::int64_t now){if(view_.connected && p.connection_id==view_.connection_id && (p.diagnostic_validity&b::counter_baseline_valid))identities(p.plugin_load_id,p.enable_id,p.aircraft_generation,now);}
std::uint64_t MotionControl::request(m::Action action,const b::LatestState& latest,std::int64_t now,m::ReconstructionSettings settings,m::SourcePreference preference,const b::SourceSelection* selection,bool exporter_ready) {
    if(!m::valid(settings))return 0;
    if(!view_.connected || !view_.identity_available || view_.transport_fault || now<0 || (action!=m::Action::follow&&action!=m::Action::stop))return 0;
    const auto& source=latest.session();
    const bool use_exporter=selection&&selection->source==m::PoseSource::exporter;
    if(action==m::Action::follow && (use_exporter?(preference==m::SourcePreference::public_only||!exporter_ready):(!(source.flags&b::synchronized) || !source.motion_epoch || !latest.available()[static_cast<std::size_t>(psx::aircraft::SourceRecord::boost)] || !latest.available()[static_cast<std::size_t>(psx::aircraft::SourceRecord::qs121)])))return 0;
    auto available=std::find_if(pending_.begin(),pending_.end(),[](const auto& p){return !p.active;});
    if(available==pending_.end() || queue_size_==queue_.size() || operation_==UINT64_MAX){++view_.control_overflows;fault(now);return 0;}
    b::MotionRequest request;request.connection_id=view_.connection_id;request.operation_id=++operation_;request.plugin_load_id=load_;request.enable_id=enable_;request.aircraft_generation=aircraft_;request.session_epoch=source.session_epoch;request.motion_epoch=source.motion_epoch;request.requested_qpc=now;request.action=action;
    request.reconstruction=settings;
    request.source_preference=preference;request.expected_selection_epoch=selection?selection->selection_epoch:1;
    b::Frame frame;if(!b::encode(request,0,frame) || !record(MotionRecordKind::request,frame,now)){fault(now);return 0;}
    *available={true,false,false,request};queue_[(queue_head_+queue_size_)%queue_.size()]=static_cast<std::size_t>(available-pending_.begin());++queue_size_;++view_.outstanding;view_.queued=queue_size_;view_.last_request=request;return request.operation_id;
}
bool MotionControl::next_request(b::MotionRequest& out) {
    if(!view_.connected || view_.transport_fault || !queue_size_)return false;auto& p=pending_[queue_[queue_head_]];queue_head_=(queue_head_+1)%queue_.size();--queue_size_;view_.queued=queue_size_;p.sent=true;out=p.request;return true;
}
bool MotionControl::acknowledge(const b::MotionAck& ack,std::int64_t received) {
    if(!view_.connected || ack.connection_id!=view_.connection_id || received<ack.generated_qpc)return false;
    auto pending=std::find_if(pending_.begin(),pending_.end(),[&](const auto& p){return p.active&&p.request.operation_id==ack.operation_id;});
    b::Frame frame;if(!b::encode(ack,0,frame))return false;
    if(pending==pending_.end()){const auto old=std::find_if(retired_.begin(),retired_.end(),[&](const auto& r){return r.operation_id==ack.operation_id && r.connection_id==ack.connection_id && r.plugin_load_id==ack.plugin_load_id && r.enable_id==ack.enable_id && r.aircraft_generation==ack.aircraft_generation;});if(old==retired_.end())return false;++view_.late_acks;if(!record(MotionRecordKind::ack,frame,received)){fault(received);return false;}return true;}
    const auto& r=pending->request;if(!pending->sent || ack.plugin_load_id!=r.plugin_load_id || ack.enable_id!=r.enable_id || ack.aircraft_generation!=r.aircraft_generation || ack.generated_qpc<r.requested_qpc)return false;
    if(ack.result==m::AckResult::accepted && pending->accepted)return false;
    if(ack.result==m::AckResult::applied && !pending->accepted)return false;
    if(!record(MotionRecordKind::ack,frame,received)){fault(received);return false;}
    view_.last_ack=ack;view_.last_ack_local=false;
    if(ack.result==m::AckResult::accepted)pending->accepted=true;
    else {remember(pending->request);*pending={};--view_.outstanding;}
    return true;
}
bool MotionControl::status(const b::MotionStatus& value,std::int64_t received) {
    if(!view_.connected || value.connection_id!=view_.connection_id || received<value.generated_qpc)return false;
    b::Frame frame;if(!b::encode(value,0,frame))return false;
    if(!identities(value.plugin_load_id,value.enable_id,value.aircraft_generation,received))return true;
    if(view_.status_available && value.revision<view_.status.revision)return false;
    // Status samples may be coalesced, but every observed phase/reason/ownership
    // transition is retained in the bounded record handoff.
    const bool transition=!view_.status_available || value.phase!=view_.status.phase || value.reason!=view_.status.reason || value.flags!=view_.status.flags || value.discontinuity_generation!=view_.status.discontinuity_generation || value.reconstruction_trace.state!=view_.status.reconstruction_trace.state || value.reconstruction.mode!=view_.status.reconstruction.mode || value.reconstruction.delay_ms!=view_.status.reconstruction.delay_ms || value.selection_epoch!=view_.status.selection_epoch || value.source_preference!=view_.status.source_preference;
    if(transition && !record(MotionRecordKind::status,frame,received)){fault(received);return false;}
    view_.status=value;view_.status_available=true;view_.status_received_qpc=received;return true;
}
}
