#include "coordinator/observation_capture.hpp"
#include "coordinator/win_handle.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <chrono>
#include <psx/sync/runtime.hpp>

namespace coordinator {
namespace b=psx::bridge;
namespace m=psx::motion;
namespace {
using Bytes=std::vector<std::uint8_t>;
void u(Bytes& b,std::uint64_t v,std::size_t n) {for(std::size_t i=0;i<n;++i)b.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
std::uint64_t get(const std::uint8_t* p,std::size_t n) {std::uint64_t v=0;for(std::size_t i=0;i<n;++i)v|=std::uint64_t(p[i])<<(i*8);return v;}
constexpr std::array<std::uint32_t,256> crc_table() {
    std::array<std::uint32_t,256> table{};
    for(std::size_t i=0;i<table.size();++i){auto value=static_cast<std::uint32_t>(i);for(unsigned bit=0;bit<8;++bit)value=(value>>1)^((value&1)?0xedb88320u:0);table[i]=value;}
    return table;
}
std::uint32_t crc(const std::uint8_t* p,std::size_t n,std::uint32_t c=0xffffffffu) {static constexpr auto table=crc_table();for(std::size_t i=0;i<n;++i)c=(c>>8)^table[(c^p[i])&255];return c;}
Bytes packet(CaptureKind kind,std::uint64_t order,std::int64_t time,const Bytes& payload) {
    Bytes result;result.reserve(28+payload.size());u(result,payload.size(),4);u(result,static_cast<unsigned>(kind),4);u(result,order,8);u(result,static_cast<std::uint64_t>(time),8);
    const std::uint32_t checksum=~crc(payload.data(),payload.size(),crc(result.data(),result.size()));
    u(result,checksum,4);result.insert(result.end(),payload.begin(),payload.end());return result;
}
void add_frame(Bytes& bytes,const b::Frame& frame) {bytes.insert(bytes.end(),frame.bytes.begin(),frame.bytes.begin()+frame.size);}
void real(Bytes& bytes,double value){std::uint64_t bits{};std::memcpy(&bits,&value,8);u(bytes,bits,8);}
double read_real(const std::uint8_t* p){const auto bits=get(p,8);double value{};std::memcpy(&value,&bits,8);return value;}
// Capture-only checkpoint. Unlike live MotionWindow this must also represent
// empty/disconnected and pitch-only startup history without invented domains.
Bytes history_bytes(const m::MotionHistory& history) {
    const auto& w=history.window();const auto& c=history.counters();Bytes bytes;
    bytes.reserve(109+33*(w.pitch_count+(w.has_pitch_anchor?1:0))+79*w.boost_count);
    for(auto v:{w.domain.session_epoch,w.domain.configuration_epoch,w.domain.boost_generation,w.domain.motion_epoch,w.domain.sequence_floor,w.revision})u(bytes,v,8);
    u(bytes,w.boost_count,2);u(bytes,w.pitch_count,2);u(bytes,w.has_pitch_anchor?1:0,1);
    for(auto v:{c.decoded_boost,c.decoded_pitch,c.evicted_boost,c.evicted_pitch,c.equal_time_boost,c.rejected,c.resets})u(bytes,v,8);
    const auto stamp=[&](const psx::aircraft::SampleStamp& s){u(bytes,s.configuration_epoch,8);u(bytes,s.sequence,8);u(bytes,static_cast<std::uint64_t>(s.received_qpc),8);};
    const auto pitch=[&](const m::PitchSample& p){stamp(p.stamp);real(bytes,p.pitch_deg);u(bytes,static_cast<unsigned>(p.pitch_reference),1);};
    if(w.has_pitch_anchor)pitch(w.pitch_anchor);
    for(std::size_t i=0;i<w.boost_count;++i){const auto& v=w.boost[i];stamp(v.stamp);for(double x:{v.geo.latitude_deg,v.geo.longitude_deg,v.geo.altitude_msl_m,v.geo.heading_deg,v.geo.pitch_deg,v.geo.bank_deg})real(bytes,x);u(bytes,v.source_millisecond_mod1000,2);u(bytes,v.has_source_millisecond?1:0,1);u(bytes,v.on_ground?1:0,1);u(bytes,static_cast<unsigned>(v.position_reference),1);u(bytes,static_cast<unsigned>(v.heading_reference),1);u(bytes,static_cast<unsigned>(v.pitch_reference),1);}
    for(std::size_t i=0;i<w.pitch_count;++i)pitch(w.pitch[i]);return bytes;
}
bool restore_history(ObservationSession& model,const std::uint8_t* p,std::size_t size) {
    if(size<109)return false;m::SampleWindow w;m::HistoryCounters c;
    w.domain={get(p,8),get(p+8,8),get(p+16,8),get(p+24,8),get(p+32,8)};w.revision=get(p+40,8);
    w.boost_count=static_cast<std::uint16_t>(get(p+48,2));w.pitch_count=static_cast<std::uint16_t>(get(p+50,2));w.has_pitch_anchor=p[52]!=0;
    if(p[52]>1 || w.boost_count>m::history_capacity || w.pitch_count>m::history_capacity || size!=109+79*std::size_t(w.boost_count)+33*(std::size_t(w.pitch_count)+(w.has_pitch_anchor?1:0)))return false;
    c={get(p+53,8),get(p+61,8),get(p+69,8),get(p+77,8),get(p+85,8),get(p+93,8),get(p+101,8)};std::size_t offset=109;bool valid=true;
    const auto stamp=[&](psx::aircraft::SampleStamp& s){s={w.domain.session_epoch,get(p+offset,8),get(p+offset+8,8),static_cast<std::int64_t>(get(p+offset+16,8))};offset+=24;};
    const auto pitch=[&](m::PitchSample& v){stamp(v.stamp);v.pitch_deg=read_real(p+offset);v.pitch_reference=static_cast<psx::aircraft::PitchReference>(p[offset+8]);offset+=9;};
    if(w.has_pitch_anchor)pitch(w.pitch_anchor);
    for(std::size_t i=0;i<w.boost_count;++i){auto& v=w.boost[i];stamp(v.stamp);v.geo={read_real(p+offset),read_real(p+offset+8),read_real(p+offset+16),read_real(p+offset+24),read_real(p+offset+32),read_real(p+offset+40)};offset+=48;v.source_millisecond_mod1000=static_cast<std::uint16_t>(get(p+offset,2));v.has_source_millisecond=p[offset+2]!=0;v.on_ground=p[offset+3]!=0;valid&=p[offset+2]<=1 && p[offset+3]<=1;v.position_reference=static_cast<psx::aircraft::PositionReference>(p[offset+4]);v.heading_reference=static_cast<psx::aircraft::HeadingReference>(p[offset+5]);v.pitch_reference=static_cast<psx::aircraft::PitchReference>(p[offset+6]);offset+=7;}
    for(std::size_t i=0;i<w.pitch_count;++i)pitch(w.pitch[i]);return valid && model.restore_history(w,c);
}
void add_history(Bytes& bytes,const ObservationSession& model){const auto history=history_bytes(model.history());u(bytes,history.size(),4);bytes.insert(bytes.end(),history.begin(),history.end());}
void add_exporter(Bytes& bytes,const ExporterModel& model,std::uint16_t wire=b::protocol_version){const auto state=exporter_checkpoint(model,wire);u(bytes,state.size(),4);bytes.insert(bytes.end(),state.begin(),state.end());}
void add_audio(Bytes& bytes,const AudioModel& model){const auto state=model.checkpoint();u(bytes,state.size(),4);bytes.insert(bytes.end(),state.begin(),state.end());}
bool write_block(HANDLE file,const void* data,DWORD length,std::uint64_t offset,DWORD& error) {
    WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.valid()){error=GetLastError();return false;}
    OVERLAPPED op{};op.hEvent=event.get();op.Offset=static_cast<DWORD>(offset);op.OffsetHigh=static_cast<DWORD>(offset>>32);
    DWORD written=0;BOOL ok=WriteFile(file,data,length,&written,&op);
    if(!ok && GetLastError()==ERROR_IO_PENDING) {
        const auto wait=WaitForSingleObject(event.get(),5000);
        if(wait!=WAIT_OBJECT_0) {error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();CancelIoEx(file,&op);GetOverlappedResult(file,&op,&written,TRUE);return false;}
        ok=GetOverlappedResult(file,&op,&written,FALSE);
    }
    if(!ok || written!=length) {error=ok?ERROR_WRITE_FAULT:GetLastError();return false;}return true;
}
template<class T> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,T& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode(p+b::header_size,h.payload_size,out);
}
template<> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,b::PluginStatus& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode_capture_plugin(p+b::header_size,h.payload_size,h.version,out);
}
template<> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,b::SessionStatus& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode_capture_session(p+b::header_size,h.payload_size,h.version,out);
}
template<> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,b::MotionRequest& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode_capture_motion_request(p+b::header_size,h.payload_size,h.version,out);
}
template<> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,b::MotionStatus& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode_capture_motion_status(p+b::header_size,h.payload_size,h.version,out);
}
template<> bool frame_value(const std::uint8_t* p,std::size_t size,b::Kind kind,b::MotionWindow& out) {
    b::Header h;return size>=b::header_size && b::read_capture_header(p,b::header_size,h) && h.kind==kind && size==b::header_size+h.payload_size && b::decode_capture_motion_window(p+b::header_size,h.payload_size,h.version,out);
}
CaptureCutoff cutoff_of(const ObservationSession& model,std::uint64_t id,std::int64_t time) {
    CaptureCutoff c;c.acknowledged=true;c.capture_id=id;c.owner_qpc=time;c.ingestion_sequence=model.sequence();
    const auto& s=model.latest().session();c.session_epoch=s.session_epoch;c.configuration_epoch=s.configuration_epoch;c.boost_generation=s.boost_generation;
    c.motion_epoch=s.motion_epoch;c.motion_sequence_floor=s.motion_sequence_floor;c.motion_boundary=s.motion_boundary;
    for(std::size_t i=1;i<b::source_slots;++i)if(model.latest().available()[i]) {
        const auto& m=model.latest().records()[i];const auto& stamp=m.observation.stamp;
        c.sources[i]={stamp.sequence,stamp.session_epoch,stamp.configuration_epoch,m.boost_generation};
    }return c;
}
Bytes cutoff_bytes(const CaptureCutoff& c,bool motion=true) {
    Bytes bytes;u(bytes,c.capture_id,8);u(bytes,static_cast<std::uint64_t>(c.owner_qpc),8);u(bytes,c.ingestion_sequence,8);
    u(bytes,c.session_epoch,8);u(bytes,c.configuration_epoch,8);u(bytes,c.boost_generation,8);
    for(const auto& s:c.sources){u(bytes,s.sequence,8);u(bytes,s.session_epoch,8);u(bytes,s.configuration_epoch,8);u(bytes,s.boost_generation,8);}
    if(motion){u(bytes,c.motion_epoch,8);u(bytes,c.motion_sequence_floor,8);u(bytes,static_cast<unsigned>(c.motion_boundary),1);}return bytes;
}
Bytes boundary_bytes(const ObservationSession& model,const LineResult& result,std::uint16_t version=b::protocol_version) {
    b::Frame frame;if(!b::encode(model.latest().session(),0,frame))throw std::logic_error("boundary session encoding");Bytes payload;add_frame(payload,frame);u(payload,model.sequence(),8);
    real(payload,result.jump_horizontal_m);real(payload,result.jump_vertical_m);real(payload,result.jump_interval_s);real(payload,result.jump_rate);payload[4]=static_cast<std::uint8_t>(version);payload[5]=static_cast<std::uint8_t>(version>>8);return payload;
}
}
struct ObservationCapture::Seed {
    ObservationSession model;
    ExporterModel exporter;
    AudioModel audio;
    bool public_window{},complete_window{};
    std::uint64_t frequency{};
    std::int64_t time{};
};
const char* capture_stop_reason(CaptureStopReason reason) noexcept {
    constexpr const char* names[]={"none","cancelled without cutoff","capture queue limit","capture file limit","file I/O","capture setup","exporter handoff loss","exporter evidence loss","audio handoff loss","audio evidence loss","audio transition overflow","audio lease handoff","motion evidence overflow","network evidence overflow","sidecar evidence incomplete"};
    const auto i=static_cast<unsigned>(reason);return i<std::size(names)?names[i]:"unknown";
}
ObservationCapture::ObservationCapture(CaptureLimits limits,psx::diagnostics::DiagnosticsHub* diagnostics):seed_(std::make_unique<Seed>()),limits_(limits),ring_(limits.queue_bytes),file_events_(diagnostics?diagnostics->producer(3,psx::diagnostics::Component::capture):psx::diagnostics::DiagnosticProducer{}) {}
ObservationCapture::~ObservationCapture(){stop();join();}
bool ObservationCapture::prepare(const std::wstring& path) {
    if(!done_||!std::filesystem::path(path).is_absolute()||limits_.queue_bytes<64||limits_.file_bytes<53)return false;
    std::lock_guard<std::mutex> worker_lock(worker_mutex_);if(!done_)return false;
    if(worker_.joinable())worker_.join();const auto identity=psx::diagnostics::new_identity();
    {std::lock_guard<std::mutex> lock(mutex_);prepared_path_=path;seed_ready_=cancel_prepared_=false;evidence_probe_=nullptr;evidence_context_=nullptr;head_=size_=0;ordinal_=seed_records_=0;accepted_=24;status_={};status_.capture_id=identity;done_=false;}
    try{worker_=std::thread([this,path]{
        try{
            {std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return seed_ready_||cancel_prepared_;});if(cancel_prepared_&&!seed_ready_){done_=true;return;}}
            run(path,seed_->frequency,seed_->time);
        }catch(...){std::lock_guard<std::mutex> lock(mutex_);status_.active=false;recording_=false;status_.incomplete=true;status_.error=ERROR_UNHANDLED_EXCEPTION;status_.stop_reason=CaptureStopReason::setup;}
        done_=true;
    });}catch(...){done_=true;std::lock_guard<std::mutex> lock(mutex_);status_.incomplete=true;status_.error=ERROR_NOT_ENOUGH_MEMORY;status_.stop_reason=CaptureStopReason::setup;return false;}
    return true;
}
bool ObservationCapture::evidence_completion(CaptureEvidenceProbe probe,void* context) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(done_||seed_ready_||cancel_prepared_)return false;
    evidence_probe_=probe;evidence_context_=context;return true;
}
bool ObservationCapture::start(const std::wstring& path,const ObservationSession& model,std::uint64_t frequency,std::int64_t now,const ExporterModel* exporter,const AudioModel* audio) {
    if(!frequency||now<0)return false;
    // Standalone offline callers can prepare here; production prepares on UI owner.
    if(done_&&!prepare(path))return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if(prepared_path_!=path||seed_ready_||cancel_prepared_||done_)return false;
    seed_->model=model;seed_->exporter=exporter?*exporter:ExporterModel(frequency);seed_->audio=audio?*audio:AudioModel(frequency);seed_->frequency=frequency;seed_->time=now;
    b::MotionWindow window;window.connection_id=1;window.session_revision=model.latest().session().revision;window.published_qpc=now;window.samples=model.history().window();window.selection_epoch=seed_->exporter.state().selection.selection_epoch;
    seed_->public_window=b::matches(window,model.latest().session());seed_->complete_window=exporter&&exporter->history().window().count;
    seed_records_=1;for(bool available:model.latest().available())if(available)++seed_records_;
    seed_records_+=seed_->public_window?1:0;seed_records_+=seed_->complete_window?1:0;
    remote_=psx::sync::enabled();network_checkpoint_=0;network_connection_=0;if(auto* endpoint=psx::sync::runtime()){endpoint->capture(true);network_lost_=endpoint->status().capture_lost;}
    ordinal_=seed_records_;frequency_=frequency;status_.records=ordinal_;status_.active=true;recording_=true;seed_ready_=true;ready_.notify_one();return true;
}
bool ObservationCapture::append(CaptureKind kind,std::int64_t qpc,const Bytes& payload) {
    if(!recording_ || payload.size()>132000) return false;
    std::lock_guard<std::mutex> lock(mutex_);if(!status_.active)return false;
    auto bytes=packet(kind,ordinal_+1,qpc,payload);
    if(bytes.size()>ring_.size()-size_ || accepted_+bytes.size()+29>limits_.file_bytes) {
        status_.active=false;recording_=false;status_.incomplete=true;++status_.overflows;status_.error=ERROR_BUFFER_OVERFLOW;status_.stop_reason=bytes.size()>ring_.size()-size_?CaptureStopReason::queue_limit:CaptureStopReason::file_limit;ready_.notify_one();return false;
    }
    const auto tail=(head_+size_)%ring_.size();const auto first=(std::min)(bytes.size(),ring_.size()-tail);
    std::memcpy(ring_.data()+tail,bytes.data(),first);std::memcpy(ring_.data(),bytes.data()+first,bytes.size()-first);
    size_+=bytes.size();accepted_+=bytes.size();++ordinal_;status_.records=ordinal_;status_.queued=size_;ready_.notify_one();return true;
}
void ObservationCapture::event(SessionEvent event,std::int64_t time){if(recording_)append(CaptureKind::event,time,Bytes{static_cast<std::uint8_t>(event)});}
void ObservationCapture::line(SourceStream stream,std::string_view line,std::int64_t received,std::int64_t decoded) {
    if(!recording_ || !ObservationSession::selected(stream,line))return;
    Bytes payload;payload.reserve(8+line.size());u(payload,static_cast<std::uint64_t>(decoded),8);payload.insert(payload.end(),line.begin(),line.end());
    append(stream==SourceStream::main?CaptureKind::main:CaptureKind::boost,received,payload);
}
void ObservationCapture::plugin(const b::PluginStatus& status,std::int64_t time){if(!recording_)return;b::Frame frame;if(b::encode(status,0,frame)){Bytes payload;add_frame(payload,frame);append(CaptureKind::plugin,time,payload);}}
void ObservationCapture::diagnostics(const b::DiagnosticBatch& batch,std::int64_t time){if(!recording_)return;b::Frame frame;if(b::encode(batch,0,frame)){Bytes payload;add_frame(payload,frame);append(CaptureKind::diagnostics,time,payload);}}
void ObservationCapture::motion(const MotionRecord& value){if(!recording_)return;Bytes payload;if(value.kind==MotionRecordKind::ack)u(payload,value.local?1:0,1);add_frame(payload,value.frame);append(static_cast<CaptureKind>(value.kind),value.received_qpc,payload);}
void ObservationCapture::motion_status(const b::MotionStatus& value,std::int64_t time){if(!recording_)return;b::Frame frame;if(b::encode(value,0,frame))motion({MotionRecordKind::status,false,time,frame});}
void ObservationCapture::boundary(const ObservationSession& model,const LineResult& result,std::int64_t time) {
    if(recording_)append(CaptureKind::motion_boundary,time,boundary_bytes(model,result));
}
void ObservationCapture::window(const ObservationSession& model,std::int64_t time,std::uint64_t selection_epoch) {
    if(!recording_)return;b::MotionWindow value;value.connection_id=1;value.session_revision=model.latest().session().revision;value.published_qpc=time;value.samples=model.history().window();
    value.selection_epoch=selection_epoch;b::Frame frame;if(b::matches(value,model.latest().session()) && b::encode(value,0,frame)){Bytes payload;add_frame(payload,frame);append(CaptureKind::motion_window,time,payload);}
}
void ObservationCapture::finish(const ObservationSession& model,std::int64_t time,const ExporterModel* exporter,const AudioModel* audio) {
    if(!recording_)return;if(auto* e=psx::sync::runtime())e->capture(false);network();if(!recording_)return;const auto c=cutoff_of(model,status().capture_id,time);
    auto bytes=cutoff_bytes(c);add_history(bytes,model);add_exporter(bytes,exporter?*exporter:ExporterModel(frequency_));add_audio(bytes,audio?*audio:AudioModel(frequency_));if(append(CaptureKind::cutoff,time,bytes)){std::lock_guard<std::mutex> lock(mutex_);status_.cutoff=c;status_.active=false;recording_=false;ready_.notify_one();}
}
void ObservationCapture::exporter_event(const ExporterEvent& event,std::uint64_t configuration){
    if(!recording_)return;Bytes data;u(data,static_cast<unsigned>(event.kind),1);u(data,event.process.pid,4);u(data,event.process.creation_filetime,8);u(data,event.client_pid,4);u(data,event.error,4);u(data,configuration,8);u(data,event.frame.size,4);data.insert(data.end(),event.frame.bytes.begin(),event.frame.bytes.begin()+event.frame.size);append(CaptureKind::exporter_event,event.received_qpc,data);
}
void ObservationCapture::exporter_input(bool public_ready,m::SourcePreference preference,m::ReconstructionSettings settings,std::int64_t time){if(!recording_)return;Bytes data;u(data,public_ready?1:0,1);u(data,static_cast<unsigned>(preference),1);u(data,static_cast<unsigned>(settings.mode),1);u(data,settings.delay_ms,2);append(CaptureKind::exporter_input,time,data);}
void ObservationCapture::selection(const b::SourceSelection& value,std::int64_t time){if(!recording_)return;b::Frame frame;if(b::encode(value,0,frame)){Bytes data;add_frame(data,frame);append(CaptureKind::source_selection,time,data);}}
void ObservationCapture::complete_window(const ExporterModel& model,std::int64_t time){if(!recording_||!model.history().window().count)return;b::CompletePoseWindow value;value.connection_id=1;value.selection_epoch=model.state().selection.selection_epoch;value.published_qpc=time;value.samples=model.history().window();b::Frame frame;if(b::encode(value,0,frame)){Bytes data;add_frame(data,frame);append(CaptureKind::complete_pose_window,time,data);}}
void ObservationCapture::audio_input(const AudioInput& in){if(recording_)append(CaptureKind::audio_input,in.now,encode_audio_input(in));}
void ObservationCapture::audio_frame(const b::Frame& frame,std::int64_t time){if(recording_){Bytes bytes;add_frame(bytes,frame);append(CaptureKind::audio_frame,time,bytes);}}
void ObservationCapture::audio_lease(const psx::audio::Lease& lease,std::int64_t time){if(recording_){psx::exporter::Frame frame;if(psx::exporter::encode(lease,0,frame)){Bytes bytes(frame.bytes.begin(),frame.bytes.begin()+frame.size);append(CaptureKind::audio_lease,time,bytes);}}}
void ObservationCapture::observe_network_loss(std::uint64_t lost){
    if(!recording_||lost==network_lost_)return;
    // Latest-only detail loss limits replay completeness, while subsequent
    // observations remain useful for diagnosing the same live incident.
    // Keep the existing queue/file bounds and an explicitly incomplete footer.
    std::lock_guard<std::mutex> lock(mutex_);status_.incomplete=true;status_.stop_reason=CaptureStopReason::network_evidence;
    status_.error=ERROR_BUFFER_OVERFLOW;++status_.overflows;network_lost_=lost;
}
void ObservationCapture::network(){
    if(!remote_||!recording_)return;auto* endpoint=psx::sync::runtime();if(!endpoint){stop(CaptureStopReason::network_evidence);return;}
    const auto context=endpoint->context();const auto& s=context.status;const auto now=qpc_now();
    observe_network_loss(s.capture_lost);
    if(!network_checkpoint_||s.connection!=network_connection_||now-network_checkpoint_>=static_cast<std::int64_t>(frequency_/4)){
        const auto& clock=context.clock;std::array<psx::sync::ClockExchange,17> samples{};const auto count=clock.checkpoint(samples);Bytes data;
        u(data,2,4);u(data,(s.running?1:0)|(s.connected?2:0)|(s.clock_valid?4:0),4);u(data,s.connection,8);u(data,clock.generation(),8);u(data,s.local_frequency,8);u(data,s.remote_frequency,8);
        for(const auto& id:{s.local,s.peer}){u(data,id.instance_high,8);u(data,id.instance_low,8);u(data,id.process_birth,8);u(data,id.pid,4);}u(data,count,4);
        for(unsigned i=0;i<count;++i){const auto& x=samples[i];for(auto v:{x.t1,x.t2,x.t3,x.t4,x.local_frequency,x.remote_frequency})u(data,v,8);}
        for(auto v:{s.queued,s.sent,s.received,s.replaced,s.expired,s.rejected,s.reconnects,s.capture_lost})u(data,v,8);
        u(data,s.ready_services,4);for(unsigned i=1;i<8;++i){u(data,s.local_services[i],4);u(data,s.remote_services[i],4);u(data,s.service_generations[i],8);}
        append(CaptureKind::network_checkpoint,now,data);network_checkpoint_=now;network_connection_=s.connection;
    }
    psx::sync::Received r;for(unsigned budget=0;budget<128&&endpoint->evidence(r);++budget){psx::sync::Frame wire;if(!psx::sync::encode(r.header,std::span(r.payload).first(r.size),wire)){stop(CaptureStopReason::network_evidence);return;}Bytes data;u(data,1,4);u(data,r.transmitted?1:0,4);u(data,static_cast<std::uint64_t>(r.received_qpc),8);data.insert(data.end(),wire.bytes.begin(),wire.bytes.begin()+wire.size);append(CaptureKind::network_packet,r.received_qpc,data);}
}
void ObservationCapture::stop(CaptureStopReason reason) noexcept {if(remote_)if(auto* e=psx::sync::runtime())e->capture(false);std::lock_guard<std::mutex> lock(mutex_);if(status_.active){status_.incomplete=true;status_.stop_reason=reason;}if(status_.active||reason==CaptureStopReason::cancelled||reason==CaptureStopReason::setup)cancel_prepared_=true;status_.active=false;recording_=false;ready_.notify_one();}
void ObservationCapture::join(){std::lock_guard<std::mutex> lock(worker_mutex_);if(worker_.joinable())worker_.join();}
CaptureStatus ObservationCapture::status() const {std::lock_guard<std::mutex> lock(mutex_);auto result=status_;result.finalizing=seed_ready_&&!result.active&&!done_;return result;}
void ObservationCapture::run(std::wstring path,std::uint64_t frequency,std::int64_t time) {
    WinHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,nullptr));
    DWORD error=0;std::uint64_t offset=0;
    const auto fail=[&](DWORD code){file_events_.emit(psx::diagnostics::Code::capture_incomplete,psx::diagnostics::Severity::error,{},psx::diagnostics::ErrorDomain::win32,code,"Capture file write failed");std::lock_guard<std::mutex> lock(mutex_);status_.active=false;recording_=false;status_.incomplete=true;status_.error=code;status_.stop_reason=CaptureStopReason::file_io;size_=0;status_.queued=0;};
    if(!file.valid()){fail(GetLastError());return;}
    Bytes header{'P','X','S','C','A','P',static_cast<std::uint8_t>(remote_?'8':'7'),0};u(header,frequency,8);u(header,static_cast<std::uint64_t>(time),8);
    if(!write_block(file.get(),header.data(),static_cast<DWORD>(header.size()),offset,error)){fail(error);return;}offset+=header.size();
    if(test_seed_delay_ms_)Sleep(test_seed_delay_ms_);
    // Serialize the immutable start checkpoint on this file owner. Live records
    // already reserve ordinals after it and can enter the bounded ring meanwhile.
    std::vector<Bytes> prefix;prefix.reserve(static_cast<std::size_t>(seed_records_));
    const auto& model=seed_->model;const auto& exporter=seed_->exporter;b::Frame frame;
    const auto add=[&](CaptureKind kind,Bytes payload){prefix.push_back(packet(kind,prefix.size()+1,time,payload));};
    if(!b::encode(model.latest().session(),0,frame))throw std::logic_error("capture seed session");
    Bytes context;u(context,model.sequence(),8);u(context,model.bootstrap(),1);u(context,model.identified()?1:0,1);add_frame(context,frame);
    const auto& detector=model.detector_checkpoint();u(context,detector.frequency,8);u(context,detector.available?1:0,1);
    real(context,detector.previous.latitude_deg);real(context,detector.previous.longitude_deg);real(context,detector.previous.altitude_msl_m);u(context,static_cast<std::uint64_t>(detector.received_qpc),8);
    add_history(context,model);add_exporter(context,exporter);add_audio(context,seed_->audio);add(CaptureKind::context,std::move(context));
    std::array<std::size_t,b::source_slots> order{};for(std::size_t i=0;i<order.size();++i)order[i]=i;
    std::sort(order.begin(),order.end(),[&](auto l,auto r){return model.latest().records()[l].observation.stamp.sequence<model.latest().records()[r].observation.stamp.sequence;});
    for(auto i:order)if(model.latest().available()[i]){
        auto message=model.latest().records()[i];message.session_revision=model.latest().session().revision;message.published_qpc=time;
        if(!b::encode(message,0,frame))throw std::logic_error("capture seed record");Bytes bytes;u(bytes,frame.size,4);add_frame(bytes,frame);const auto& raw=model.raw()[i];bytes.insert(bytes.end(),raw.text.begin(),raw.text.begin()+raw.size);add(CaptureKind::seed,std::move(bytes));
    }
    if(seed_->public_window){b::MotionWindow w;w.connection_id=1;w.session_revision=model.latest().session().revision;w.published_qpc=time;w.samples=model.history().window();w.selection_epoch=exporter.state().selection.selection_epoch;if(!b::encode(w,0,frame))throw std::logic_error("capture seed motion window");Bytes bytes;add_frame(bytes,frame);add(CaptureKind::motion_window,std::move(bytes));}
    if(seed_->complete_window){b::CompletePoseWindow w;w.connection_id=1;w.selection_epoch=exporter.state().selection.selection_epoch;w.published_qpc=time;w.samples=exporter.history().window();if(!b::encode(w,0,frame))throw std::logic_error("capture seed complete window");Bytes bytes;add_frame(bytes,frame);add(CaptureKind::complete_pose_window,std::move(bytes));}
    if(prefix.size()!=seed_records_)throw std::logic_error("capture seed ordinal reservation");
    std::uint64_t prefix_bytes=0;for(const auto& bytes:prefix)prefix_bytes+=bytes.size();
    {std::lock_guard<std::mutex> lock(mutex_);accepted_+=prefix_bytes;if(accepted_+29>limits_.file_bytes){status_.active=false;recording_=false;status_.incomplete=true;status_.stop_reason=CaptureStopReason::file_limit;status_.error=ERROR_BUFFER_OVERFLOW;++status_.overflows;size_=0;status_.queued=0;ordinal_=seed_records_;status_.cutoff={};}}
    std::uint64_t written_seeds=0;
    for(const auto& bytes:prefix){
        if(offset+bytes.size()+29>limits_.file_bytes){std::lock_guard<std::mutex> lock(mutex_);status_.active=false;recording_=false;status_.incomplete=true;status_.stop_reason=CaptureStopReason::file_limit;status_.error=ERROR_BUFFER_OVERFLOW;++status_.overflows;size_=0;status_.queued=0;ordinal_=written_seeds;status_.cutoff={};break;}
        if(!write_block(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),offset,error)){fail(error);return;}offset+=bytes.size();++written_seeds;
    }
    std::array<std::uint8_t,65536> chunk{};
    for(;;) {
        std::size_t count=0;
        {std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return size_ || !status_.active;});
            if(!size_)break;count=(std::min)(size_,chunk.size());
            const auto first=(std::min)(count,ring_.size()-head_);std::memcpy(chunk.data(),ring_.data()+head_,first);std::memcpy(chunk.data()+first,ring_.data(),count-first);head_=(head_+count)%ring_.size();size_-=count;status_.queued=size_;}
        if(!write_block(file.get(),chunk.data(),static_cast<DWORD>(count),offset,error)){fail(error);return;}offset+=count;
        {std::lock_guard<std::mutex> lock(mutex_);status_.bytes=offset;}
    }
    // Acquisition has already frozen its cutoff. Deferred sidecar open/write
    // failures can still arrive, so only this file owner waits before sealing.
    CaptureEvidenceProbe probe;void* completion_context;
    {std::lock_guard<std::mutex> lock(mutex_);probe=evidence_probe_;completion_context=evidence_context_;}
    if(probe)for(;;){const auto evidence=probe(completion_context);if(evidence.done){
        if(!evidence.complete){std::lock_guard<std::mutex> lock(mutex_);status_.incomplete=true;
            if(status_.stop_reason==CaptureStopReason::none){status_.stop_reason=CaptureStopReason::sidecar_evidence;status_.error=evidence.error?evidence.error:ERROR_INVALID_DATA;}}
        break;}
        // No SDK/acquisition/UI thread waits here. Windows may retain an I/O
        // request until its driver completes; never detach its storage early.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool complete;std::uint64_t ordinal;
    {std::lock_guard<std::mutex> lock(mutex_);complete=!status_.incomplete && status_.cutoff.acknowledged;ordinal=ordinal_+1;}
    const auto finalized=qpc_now();const auto footer=packet(CaptureKind::footer,ordinal,finalized,Bytes{static_cast<std::uint8_t>(complete?1:0)});
    if(!write_block(file.get(),footer.data(),static_cast<DWORD>(footer.size()),offset,error)){fail(error);return;}
    {std::lock_guard<std::mutex> lock(mutex_);status_.bytes=offset+footer.size();status_.complete=complete;status_.finalized_qpc=finalized;}
    file_events_.emit(complete?psx::diagnostics::Code::capture_complete:psx::diagnostics::Code::capture_incomplete,complete?psx::diagnostics::Severity::info:psx::diagnostics::Severity::warning,{},psx::diagnostics::ErrorDomain::none,0,capture_stop_reason(status().stop_reason),{status().capture_id,offset+footer.size(),ordinal});
}
ReplayResult replay_capture(const std::wstring& path,m::ReconstructionSettings inspection) {
    ReplayResult result;result.inspection_settings=inspection;
    try {
        if(!m::valid(inspection))throw std::runtime_error("inspection settings");
        std::ifstream file(std::filesystem::path(path),std::ios::binary);std::array<std::uint8_t,24> header{};
        if(!file.read(reinterpret_cast<char*>(header.data()),header.size()) || std::memcmp(header.data(),"PXSCAP",6) || header[6]<'2'||header[6]>'8'||header[7]) throw std::runtime_error("capture header");
        result.capture_version=header[6]-'0';
        result.qpc_frequency=get(header.data()+8,8);if(!result.qpc_frequency || get(header.data()+16,8)>std::uint64_t(INT64_MAX))throw std::runtime_error("capture frequency/start");
        const bool modern=result.capture_version>=4,history_capture=result.capture_version>=5,exporter_capture=result.capture_version>=6,audio_capture=result.capture_version>=7;
        const std::uint16_t wire=audio_capture?6:exporter_capture?5:history_capture?4:3;
        // Offline reconstruction owns large fixed histories. Keep those on the
        // worker heap, independent of the caller's snapshot/inspection stack.
        auto audio_owner=std::make_unique<AudioModel>(result.qpc_frequency);auto& audio=*audio_owner;
        auto model_owner=std::make_unique<ObservationSession>(result.qpc_frequency,modern);auto& model=*model_owner;
        auto exporter_owner=std::make_unique<ExporterModel>(result.qpc_frequency,exporter_capture?m::instant_reference_revision:m::legacy_reference_revision);auto& exporter=*exporter_owner;
        std::uint64_t ordinal=0;bool context=false,seeds=true;Bytes expected_boundary;
        bool expected_selection=false;std::int64_t exporter_cursor=0;
        std::int64_t cursor=0;m::HistoryDomain inspection_domain{};
        for(;;) {
            std::array<std::uint8_t,28> h{};if(!file.read(reinterpret_cast<char*>(h.data()),h.size()))throw std::runtime_error("missing footer or truncated header");
            const auto size=get(h.data(),4);const auto kind=static_cast<CaptureKind>(get(h.data()+4,4));
            if(size>132000 || get(h.data()+8,8)!=++ordinal || get(h.data()+16,8)>std::uint64_t(INT64_MAX))throw std::runtime_error("record bound/order/time");
            Bytes data(static_cast<std::size_t>(size));if(size && !file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(size)))throw std::runtime_error("truncated record");
            const std::uint32_t checksum=~crc(data.data(),data.size(),crc(h.data(),24));
            if(get(h.data()+24,4)!=checksum)throw std::runtime_error("record CRC");
            const auto time=static_cast<std::int64_t>(get(h.data()+16,8));++result.records;
            if(result.cutoff.acknowledged && kind!=CaptureKind::footer)throw std::runtime_error("record after acquisition cutoff");
            if(result.capture_version==8&&kind==CaptureKind::network_packet){
                psx::sync::Header network;std::span<const std::uint8_t> payload;
                if(data.size()<16||get(data.data(),4)!=1||get(data.data()+4,4)>1||get(data.data()+8,8)!=static_cast<std::uint64_t>(time)||!psx::sync::decode(std::span(data).subspan(16),network,payload))throw std::runtime_error("network packet provenance");++result.network_packets;continue;
            }
            if(result.capture_version==8&&kind==CaptureKind::network_checkpoint){
                if(data.size()<164||(get(data.data(),4)!=1&&get(data.data(),4)!=2)||get(data.data()+4,4)>7||get(data.data()+24,8)!=result.qpc_frequency)throw std::runtime_error("network clock checkpoint");const auto schema=get(data.data(),4),count=get(data.data()+96,4);if(count>17||data.size()!=(schema==1?164:280)+count*48)throw std::runtime_error("network clock window");psx::sync::ClockMap map;
                if(schema==2){const auto* p=data.data()+164+count*48;const auto ready=static_cast<std::uint32_t>(get(p,4));if(ready&~0xfeu)throw std::runtime_error("network service flags");for(unsigned i=1;i<8;++i){const auto* service=p+4+(i-1)*16;if((ready&(1u<<i))&&(!get(service,4)||!get(service+4,4)||!get(service+8,8)))throw std::runtime_error("network service incarnation");}}
                for(std::size_t i=0;i<count;++i){const auto* p=data.data()+100+i*48;if(!map.accept({get(p,8),get(p+8,8),get(p+16,8),get(p+24,8),get(p+32,8),get(p+40,8)}))throw std::runtime_error("network clock exchange");}++result.network_checkpoints;continue;
            }
            if(!expected_boundary.empty() && kind!=CaptureKind::motion_boundary && kind!=CaptureKind::footer)throw std::runtime_error("missing motion boundary");
            if(expected_selection&&kind!=CaptureKind::source_selection&&kind!=CaptureKind::footer)throw std::runtime_error("missing source selection");
            if(kind==CaptureKind::context) {
                b::SessionStatus status;b::Header frame_header;const std::size_t extra=modern?41:0;
                if(context || data.size()<10+b::header_size+extra || data[9]>1 || !b::read_capture_header(data.data()+10,b::header_size,frame_header))throw std::runtime_error("seed context header");
                const auto frame_size=b::header_size+frame_header.payload_size;const auto checkpoint=10+frame_size+extra;
                if(data.size()<checkpoint || (!history_capture && data.size()!=checkpoint) || (history_capture && frame_header.version!=wire) ||
                   !frame_value(data.data()+10,frame_size,b::Kind::session,status) || (!modern && status.motion_epoch) || !model.restore(status,get(data.data(),8),data[8],data[9]!=0))throw std::runtime_error("seed context");
                if(modern){const auto* p=data.data()+10+frame_size;MotionDetectorCheckpoint detector;detector.frequency=get(p,8);detector.available=p[8]!=0;detector.previous.latitude_deg=read_real(p+9);detector.previous.longitude_deg=read_real(p+17);detector.previous.altitude_msl_m=read_real(p+25);detector.received_qpc=static_cast<std::int64_t>(get(p+33,8));
                    if(p[8]>1 || (detector.frequency && detector.frequency!=result.qpc_frequency) || !model.restore_detector(detector))throw std::runtime_error("motion detector checkpoint");}
                if(history_capture){if(data.size()<checkpoint+4)throw std::runtime_error("history checkpoint length");const auto length=static_cast<std::size_t>(get(data.data()+checkpoint,4));
                    if(length>data.size()-checkpoint-4||(!exporter_capture&&length!=data.size()-checkpoint-4)||!restore_history(model,data.data()+checkpoint+4,length))throw std::runtime_error("history checkpoint");
                    if(exporter_capture){const auto offset=checkpoint+4+length;if(data.size()<offset+4)throw std::runtime_error("exporter checkpoint length");const auto n=static_cast<std::size_t>(get(data.data()+offset,4));if(n>data.size()-offset-4||(!audio_capture&&n!=data.size()-offset-4)||!restore_exporter_checkpoint(exporter,data.data()+offset+4,n))throw std::runtime_error("exporter checkpoint");
                        if(audio_capture){const auto at=offset+4+n;if(data.size()<at+4||get(data.data()+at,4)!=data.size()-at-4||!audio.restore(data.data()+at+4,data.size()-at-4))throw std::runtime_error("audio checkpoint");}}}
                context=true;
            } else if(!context) throw std::runtime_error("missing seed context");
            else if(kind==CaptureKind::seed) {
                if(!seeds || data.size()<4)throw std::runtime_error("late/short seed");
                const auto n=static_cast<std::size_t>(get(data.data(),4));b::ObservationMessage observation;
                if(n>data.size()-4 || !frame_value(data.data()+4,n,b::Kind::observation,observation) || !model.seed(observation,std::string_view(reinterpret_cast<const char*>(data.data()+4+n),data.size()-4-n)))throw std::runtime_error("seed raw/observation mismatch");
            } else if(kind==CaptureKind::event) {
                seeds=false;if(data.size()!=1 || data[0]<1 || data[0]>6)throw std::runtime_error("lifecycle record");model.event(static_cast<SessionEvent>(data[0]),time);
                if(modern && static_cast<SessionEvent>(data[0])==SessionEvent::main_up)expected_boundary=boundary_bytes(model,{},wire);
            } else if(kind==CaptureKind::main || kind==CaptureKind::boost) {
                seeds=false;if(data.size()<8 || get(data.data(),8)>std::uint64_t(INT64_MAX))throw std::runtime_error("raw timing");
                const auto outcome=model.line(kind==CaptureKind::main?SourceStream::main:SourceStream::boost,std::string_view(reinterpret_cast<const char*>(data.data()+8),data.size()-8),time,static_cast<std::int64_t>(get(data.data(),8)));
                result.selected+=outcome.selected?1:0;result.malformed+=outcome.malformed?1:0;
                if(modern && outcome.motion_boundary)expected_boundary=boundary_bytes(model,outcome,wire);
            } else if(kind==CaptureKind::plugin) {seeds=false;b::PluginStatus status;if(!frame_value(data.data(),data.size(),b::Kind::plugin_status,status))throw std::runtime_error("plugin diagnostics");}
            else if(kind==CaptureKind::diagnostics && result.capture_version>=3) {
                seeds=false;b::DiagnosticBatch batch;if(!frame_value(data.data(),data.size(),b::Kind::diagnostic_events,batch))throw std::runtime_error("diagnostic batch");result.diagnostic_events+=batch.count;
            } else if(kind==CaptureKind::motion_request && modern) {
                seeds=false;b::MotionRequest value;if(!frame_value(data.data(),data.size(),b::Kind::motion_request,value))throw std::runtime_error("motion request");++result.motion_requests;result.motion.last_request=value;
                cursor=0;
            } else if(kind==CaptureKind::motion_ack && modern) {
                seeds=false;b::MotionAck value;if(data.empty() || data[0]>1 || !frame_value(data.data()+1,data.size()-1,b::Kind::motion_ack,value) || (data[0] && value.result!=psx::motion::AckResult::cancelled))throw std::runtime_error("motion acknowledgement");++result.motion_acks;result.local_cancellations+=data[0];result.motion.last_ack=value;result.motion.last_ack_local=data[0]!=0;
            } else if(kind==CaptureKind::motion_status && modern) {
                seeds=false;b::MotionStatus value;if(!frame_value(data.data(),data.size(),b::Kind::motion_status,value))throw std::runtime_error("motion status");++result.motion_statuses;if(!result.motion.status_available || result.motion.status.discontinuity_generation!=value.discontinuity_generation)cursor=0;result.motion.status=value;result.motion.status_available=true;result.motion.status_received_qpc=time;
            } else if(kind==CaptureKind::motion_boundary && modern) {
                seeds=false;if(expected_boundary.empty() || data!=expected_boundary)throw std::runtime_error("motion boundary decision mismatch");expected_boundary.clear();++result.motion_boundaries;
            } else if(kind==CaptureKind::motion_window && history_capture) {
                seeds=false;b::MotionWindow window;if(!frame_value(data.data(),data.size(),b::Kind::motion_window,window) || window.published_qpc!=time || !b::matches(window,model.latest().session())||(exporter_capture&&window.selection_epoch!=exporter.state().selection.selection_epoch))throw std::runtime_error("motion window envelope");
                auto expected=window;expected.samples=model.history().window();b::Frame frame;if(!b::encode(expected,0,frame))throw std::runtime_error("motion window encoding");
                if(!exporter_capture){frame.size-=8;frame.bytes[4]=4;frame.bytes[5]=0;const auto payload=frame.size-b::header_size;for(std::size_t i=0;i<4;++i)frame.bytes[8+i]=static_cast<std::uint8_t>(payload>>(8*i));}
                frame.bytes[4]=static_cast<std::uint8_t>(wire);frame.bytes[5]=0;
                if(frame.size!=data.size() || !std::equal(data.begin(),data.end(),frame.bytes.begin()))throw std::runtime_error("motion window history mismatch");
                if(exporter_capture&&exporter.state().selection.source==m::PoseSource::exporter){++result.motion_windows;continue;}
                if(!m::same_motion_domain(inspection_domain,window.samples.domain)){cursor=0;inspection_domain=window.samples.domain;}
                const auto reference_revision=exporter_capture?exporter.reference_revision():m::legacy_reference_revision;
                m::ReconstructionResult sample;const bool usable=m::reconstruct(window.samples,inspection,time,result.qpc_frequency,cursor,sample,reference_revision);if(usable)cursor=sample.trace.presentation_qpc;
                ++result.motion_windows;++result.inspection_states[static_cast<unsigned>(sample.trace.state)];
                result.inspection_tail[result.inspection_samples++%result.inspection_tail.size()]=sample;result.inspection_tail_count=(std::min)(result.inspection_tail_count+1,result.inspection_tail.size());
            } else if(kind==CaptureKind::exporter_event&&exporter_capture){
                seeds=false;if(data.size()<33||data[0]<1||data[0]>(audio_capture?7:5)||get(data.data()+29,4)!=data.size()-33||data.size()-33>psx::exporter::max_frame)throw std::runtime_error("exporter event shape");
                ExporterEvent event;event.kind=static_cast<ExporterEventKind>(data[0]);event.received_qpc=time;event.process={static_cast<std::uint32_t>(get(data.data()+1,4)),get(data.data()+5,8)};
                event.client_pid=static_cast<std::uint32_t>(get(data.data()+13,4));event.error=static_cast<std::uint32_t>(get(data.data()+17,4));event.frame.size=data.size()-33;std::copy(data.begin()+33,data.end(),event.frame.bytes.begin());
                if(!exporter.event(event,get(data.data()+21,8)))throw std::runtime_error("exporter event admission");++result.exporter_events;
            } else if(kind==CaptureKind::exporter_input&&exporter_capture){
                seeds=false;if(data.size()!=5||data[0]>1||data[1]>1)throw std::runtime_error("exporter selection input");m::ReconstructionSettings settings;settings.mode=static_cast<m::ReconstructionMode>(data[2]);settings.delay_ms=static_cast<std::uint16_t>(get(data.data()+3,2));if(!m::valid(settings))throw std::runtime_error("exporter selection settings");
                exporter.preference(static_cast<m::SourcePreference>(data[1]),settings);expected_selection=exporter.select(time,data[0]!=0);
            } else if(kind==CaptureKind::source_selection&&exporter_capture){
                seeds=false;b::SourceSelection selection;if(!expected_selection||!frame_value(data.data(),data.size(),b::Kind::source_selection,selection))throw std::runtime_error("source selection control");
                b::Frame frame;if(!b::encode(exporter.state().selection,0,frame))throw std::runtime_error("source selection encoding");frame.bytes[4]=static_cast<std::uint8_t>(wire);frame.bytes[5]=0;if(frame.size!=data.size()||!std::equal(data.begin(),data.end(),frame.bytes.begin()))throw std::runtime_error("source selection decision mismatch");
                expected_selection=false;cursor=exporter_cursor=0;++result.source_selections;
            } else if(kind==CaptureKind::complete_pose_window&&exporter_capture){
                seeds=false;b::CompletePoseWindow window;if(!frame_value(data.data(),data.size(),b::Kind::complete_pose_window,window)||window.published_qpc!=time||window.selection_epoch!=exporter.state().selection.selection_epoch)throw std::runtime_error("complete window envelope");
                auto expected=window;expected.samples=exporter.history().window();b::Frame frame;if(!b::encode(expected,0,frame))throw std::runtime_error("complete window encoding");frame.bytes[4]=static_cast<std::uint8_t>(wire);frame.bytes[5]=0;if(frame.size!=data.size()||!std::equal(data.begin(),data.end(),frame.bytes.begin()))throw std::runtime_error("complete window reconstruction mismatch");++result.complete_windows;
                if(exporter.state().selection.source==m::PoseSource::exporter){m::ReconstructionResult sample;if(m::reconstruct(window.samples,inspection,time,result.qpc_frequency,exporter_cursor,sample))exporter_cursor=sample.trace.presentation_qpc;sample.pose.provenance.selection_epoch=window.selection_epoch;
                    ++result.inspection_states[static_cast<unsigned>(sample.trace.state)];result.inspection_tail[result.inspection_samples++%result.inspection_tail.size()]=sample;result.inspection_tail_count=(std::min)(result.inspection_tail_count+1,result.inspection_tail.size());}
            } else if(kind==CaptureKind::audio_input&&audio_capture){
                seeds=false;AudioInput in;if(!decode_audio_input(data.data(),data.size(),in)||in.now!=time)throw std::runtime_error("audio input");audio.apply(in);++result.audio_inputs;
            } else if(kind==CaptureKind::audio_frame&&audio_capture){
                seeds=false;b::Frame frame;if(!audio.next_frame(frame,time)||frame.size!=data.size()||!std::equal(data.begin(),data.end(),frame.bytes.begin()))throw std::runtime_error("audio output mismatch");++result.audio_frames;
            } else if(kind==CaptureKind::audio_lease&&audio_capture){
                seeds=false;psx::audio::Lease lease;psx::exporter::Frame frame;if(!audio.next_lease(lease)||!psx::exporter::encode(lease,0,frame)||frame.size!=data.size()||!std::equal(data.begin(),data.end(),frame.bytes.begin()))throw std::runtime_error("audio lease decision mismatch");++result.audio_leases;
            } else if(kind==CaptureKind::cutoff && result.capture_version>=3) {
                seeds=false;const auto base=48+b::source_slots*32+(modern?17:0);if(data.size()<base || (!history_capture && data.size()!=base) || !get(data.data(),8) || get(data.data()+8,8)!=static_cast<std::uint64_t>(time))throw std::runtime_error("cutoff shape/time");
                auto expected=cutoff_of(model,get(data.data(),8),time);auto expected_bytes=cutoff_bytes(expected,modern);if(history_capture)add_history(expected_bytes,model);if(exporter_capture)add_exporter(expected_bytes,exporter,wire);if(audio_capture)add_audio(expected_bytes,audio);if(expected_bytes!=data)throw std::runtime_error("cutoff state mismatch");result.cutoff=expected;
            } else if(kind==CaptureKind::footer) {
                if(data.size()!=1 || data[0]>1 || file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("footer/trailing data");
                if(data[0]==1 && result.capture_version>=3 && (!result.cutoff.acknowledged || !expected_boundary.empty()||expected_selection||(result.capture_version==8&&!result.network_checkpoints)))throw std::runtime_error("complete footer lacks acquisition cutoff/boundary/network provenance");
                if(result.cutoff.acknowledged && time<result.cutoff.owner_qpc)throw std::runtime_error("footer before cutoff");
                result.valid=true;result.complete=data[0]==1;result.latest=model.latest();result.motion_window=model.history().window();result.history_counters=model.history().counters();result.exporter=exporter.state();result.complete_window=exporter.history().window();result.finalized_qpc=time;result.audio=audio.view();result.readiness=inspect_readiness_capture(path+L".readiness");result.traffic=inspect_traffic_capture(path+L".traffic");if(!result.complete)result.error="capture marked incomplete";return result;
            } else throw std::runtime_error("unknown capture kind");
        }
    }catch(const std::exception& e){result.error=e.what();}return result;
}
}
