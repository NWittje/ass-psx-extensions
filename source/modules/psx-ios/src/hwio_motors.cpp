#include <ios/hwio_motors.hpp>
#include <ios/airport.hpp>
#include <ios/hwio_peer.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
#include <cmath>
namespace ios {
namespace {
namespace h=psx::hwio::motors;
namespace v2=h::v2;
static_assert(motor_slots==v2::slots);
static_assert(offsetof(h::Packet,operation)==offsetof(v2::Packet,operation) && offsetof(h::Packet,motors)==offsetof(v2::Packet,motors));
namespace v3=psx::hwio::actuators;
template<class Legacy>v3::Packet observed_legacy(const Legacy& source){v3::Packet target;std::memcpy(&target,&source,offsetof(Legacy,motors));for(unsigned n=0;n<source.motors.size();++n)std::memcpy(&target.motors[n],&source.motors[n],sizeof(h::Status));target.host=source.host;target.message=source.message;return target;}
template<class Legacy>Legacy legacy_request(const v3::Packet& source){Legacy target;constexpr auto begin=offsetof(h::Packet,operation),end=offsetof(h::Packet,motors);std::memcpy(reinterpret_cast<std::byte*>(&target)+begin,reinterpret_cast<const std::byte*>(&source)+begin,end-begin);target.host=source.host;return target;}

}
HwioMotors::HwioMotors(std::wstring endpoint,psx::non_normal::ProcessIdentity expected,std::wstring v2_endpoint,std::wstring v3_endpoint):buffers_(std::make_unique<Buffers>()),incoming_(buffers_->incoming),outgoing_(buffers_->outgoing),v2_incoming_(buffers_->v2_incoming),v2_outgoing_(buffers_->v2_outgoing),legacy_incoming_(buffers_->legacy_incoming),legacy_outgoing_(buffers_->legacy_outgoing),status_(buffers_->status),endpoint_(std::move(endpoint)),v2_endpoint_(std::move(v2_endpoint)),v3_endpoint_(std::move(v3_endpoint)),explicit_peer_(expected.pid?expected:psx::non_normal::current_process()) {next_=(GetTickCount64()<<16)^GetCurrentProcessId();if(!next_)next_=1;read_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);write_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
HwioMotors::~HwioMotors(){disconnected();if(read_.hEvent)CloseHandle(read_.hEvent);if(write_.hEvent)CloseHandle(write_.hEvent);}
std::optional<psx::hwio::situation_controls::Snapshot> HwioMotors::situation_controls() const{
  if(!available()||wire_version_!=3)return {};
  return psx::hwio::situation_controls::get(*status_,psx::non_normal::qpc(),psx::non_normal::qpc_frequency());
}
void HwioMotors::disconnected() {
  if(pipe_!=INVALID_HANDLE_VALUE) {CancelIoEx(pipe_,nullptr);DWORD n{};if(reading_)GetOverlappedResult(pipe_,&read_,&n,TRUE);if(writing_)GetOverlappedResult(pipe_,&write_,&n,TRUE);CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}
  reading_=writing_=awaiting_=false;pending_.clear();replies_.clear();status_.reset();wire_version_=0;reason_="HWIO unavailable";
}
unsigned HwioMotors::wire_slots()const{return wire_version_==3?60:wire_version_==2?v2::slots:h::slots;}
DWORD HwioMotors::wire_bytes()const{return wire_version_==3?DWORD(sizeof(Packet)):wire_version_==2?DWORD(sizeof(v2::Packet)):DWORD(sizeof(h::Packet));}
bool HwioMotors::connect() {
  if((endpoint_.empty() && v2_endpoint_.empty()) || !v3_endpoint_.empty()){
    pipe_=open_hwio_pipe(psx::hwio::actuators::pipe_prefix,v3_endpoint_,explicit_peer_);if(pipe_!=INVALID_HANDLE_VALUE)wire_version_=3;
    else if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND){reason_=GetLastError()==ERROR_ACCESS_DENIED?"HWIO pipe identity rejected":"HWIO actuator v3 unavailable";return false;}
  }
  // Explicit legacy fixtures stay isolated. Production falls back only on absent endpoints.
  if(pipe_==INVALID_HANDLE_VALUE && (endpoint_.empty() || !v2_endpoint_.empty())){
    pipe_=open_hwio_pipe(v2::pipe_prefix,v2_endpoint_,explicit_peer_);
    if(pipe_!=INVALID_HANDLE_VALUE)wire_version_=2;
    else if(GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND){reason_=GetLastError()==ERROR_ACCESS_DENIED?"HWIO pipe identity rejected":"HWIO motor v2 unavailable";return false;}
  }
  if(pipe_==INVALID_HANDLE_VALUE){pipe_=open_hwio_pipe(h::pipe_prefix,endpoint_,explicit_peer_);if(pipe_!=INVALID_HANDLE_VALUE)wire_version_=1;}
  if(pipe_==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_ACCESS_DENIED)reason_="HWIO pipe identity rejected";return false;}
  if(connection_==UINT64_MAX){disconnected();reason_="HWIO connection identity exhausted";return false;}
  ++connection_;
  Packet p;p.operation=h::Operation::status;send(p);reason_="Waiting for HWIO status";return true;
}
std::uint64_t HwioMotors::send(Packet p) {
  if(p.operation<h::Operation::status || p.operation>h::Operation::stop || (p.operation!=h::Operation::status && (p.motor>=std::min(20u,wire_slots()) || unsigned(p.kind)>2 || (wire_version_<3 && p.kind!=psx::hwio::actuators::Kind::motor))))return 0;
  if(pipe_==INVALID_HANDLE_VALUE||pending_.size()>=32||next_==UINT64_MAX)return 0;
  p.request_id=next_++;if(status_)p.session=status_->session;pending_.push_back(p);return p.request_id;
}
bool HwioMotors::receive(std::uint64_t id,Packet &p) {
  auto i=std::find_if(replies_.begin(),replies_.end(),[&](const auto &r){return r.request_id==id;});if(i==replies_.end())return false;
  p=*i;replies_.erase(i);return true;
}
bool HwioMotors::available() const{return pipe_!=INVALID_HANDLE_VALUE && status_.has_value() && status_->session && status_->result==h::Result::ok;}
void HwioMotors::accept_read(){if(wire_version_==3)accept(incoming_);else if(wire_version_==2)accept(observed_legacy(v2_incoming_));else accept(observed_legacy(legacy_incoming_));}
void HwioMotors::accept(const Packet& p){
  if(p.signature!=(wire_version_==3?0x33544148u:wire_version_==2?0x32544d48u:0x31544d48u) || p.version!=wire_version_ || p.bytes!=wire_bytes() || p.request_id!=outgoing_.request_id || p.operation!=outgoing_.operation || p.motor!=outgoing_.motor || (wire_version_==3 && p.kind!=outgoing_.kind) || unsigned(p.result)>unsigned(h::Result::busy)){disconnected();reason_="Malformed motor reply; no replay";return;}
  if(!p.session || (p.operation==h::Operation::status && p.result!=h::Result::ok)){disconnected();reason_="HWIO motor status unavailable";return;}
  if(status_ && p.session!=status_->session){disconnected();reason_="HWIO motor session changed; no replay";return;}
  if(hwio_endpoint(hwio_text(p.host))!=hwio_endpoint(host_) || p.port!=port_){disconnected();reason_="HWIO uses another PSX endpoint";return;}
  if(wire_version_==3)for(const auto& item:p.motors)if(!std::isfinite(item.position) || !std::isfinite(item.target) || !std::isfinite(item.minimum) || !std::isfinite(item.maximum) || unsigned(item.reference)>3){disconnected();reason_="Malformed actuator feedback; no replay";return;}
  awaiting_=false;status_=p;reason_=hwio_text(p.message);
  if(p.operation!=h::Operation::status){if(replies_.size()>=32)replies_.pop_front();replies_.push_back(p);}
}
void HwioMotors::io(Tick t) {
  if(pipe_==INVALID_HANDLE_VALUE)return;
  DWORD n{};
  if(writing_) {
    if(GetOverlappedResult(pipe_,&write_,&n,FALSE)) {writing_=false;if(n!=wire_bytes()){disconnected();return;}}
    else if(GetLastError()!=ERROR_IO_INCOMPLETE){disconnected();return;}
  }
  if(reading_) {
    if(GetOverlappedResult(pipe_,&read_,&n,FALSE)) {reading_=false;if(n!=wire_bytes()){disconnected();return;}accept_read();}
    else if(GetLastError()!=ERROR_IO_INCOMPLETE){disconnected();return;}
  }
  if(pipe_==INVALID_HANDLE_VALUE)return;
  if(awaiting_&&t>=reply_deadline_){disconnected();reason_="HWIO reply unavailable; no replay";return;}
  if(!awaiting_&&!writing_&&!pending_.empty()) {
    outgoing_=pending_.front();pending_.pop_front();awaiting_=true;reply_deadline_=t+2000;
    outgoing_.signature=wire_version_==3?0x33544148u:wire_version_==2?0x32544d48u:0x31544d48u;outgoing_.version=wire_version_;outgoing_.bytes=std::uint16_t(wire_bytes());
    ResetEvent(write_.hEvent);write_.Internal=write_.InternalHigh=0;write_.Offset=write_.OffsetHigh=0;
    if(wire_version_==1)legacy_outgoing_=legacy_request<h::Packet>(outgoing_);else if(wire_version_==2)v2_outgoing_=legacy_request<v2::Packet>(outgoing_);
    const void* bytes=wire_version_==3?static_cast<const void*>(&outgoing_):wire_version_==2?static_cast<const void*>(&v2_outgoing_):static_cast<const void*>(&legacy_outgoing_);
    if(WriteFile(pipe_,bytes,wire_bytes(),&n,&write_)) {if(n!=wire_bytes()){disconnected();return;}}
    else if(GetLastError()==ERROR_IO_PENDING)writing_=true;else{disconnected();return;}
  }
  if(awaiting_&&!reading_) {
    ResetEvent(read_.hEvent);read_.Internal=read_.InternalHigh=0;read_.Offset=read_.OffsetHigh=0;
    void* bytes=wire_version_==3?static_cast<void*>(&incoming_):wire_version_==2?static_cast<void*>(&v2_incoming_):static_cast<void*>(&legacy_incoming_);
    if(ReadFile(pipe_,bytes,wire_bytes(),&n,&read_)) {if(n!=wire_bytes()){disconnected();return;}accept_read();}
    else if(GetLastError()==ERROR_IO_PENDING)reading_=true;else disconnected();
  }
}
void HwioMotors::poll(Tick t,std::string host,std::uint16_t port){
  if(host!=host_ || port!=port_){disconnected();host_=std::move(host);port_=port;connect_at_=0;}
  if(host_.size()>=128){reason_="PSX endpoint exceeds motor contract bounds";return;}
  if(pipe_==INVALID_HANDLE_VALUE && t>=connect_at_){connect_at_=t+1000;connect();status_at_=t+200;}
  io(t);
  if(pipe_!=INVALID_HANDLE_VALUE && t>=status_at_ && pending_.size()<4){Packet p;send(p);status_at_=t+200;}
}
void HwioMotors::command(Operation& op,Tick t,bool first){
  const auto index=field<std::uint64_t>(*op.command,"motor");
  if(index>=(op.kind.starts_with("actuator-")?60u:20u)){op.state="failed";op.detail="Invalid motor index";return;}
  const auto selected=unsigned(index);auto& action=actions_[selected];
  if(first){
    if(!available()){op.state="failed";op.detail=reason_;return;}
    if(selected>=wire_slots()){op.state="failed";op.detail=selected>=20?"Typed actuator requires HWIO actuator pipe v3":"Motor "+std::to_string(selected)+" requires HWIO motor pipe v2";return;}
    const auto motor=field<std::uint64_t>(*op.command,"motor");
    if(motor>=wire_slots() || field<std::uint64_t>(*op.command,"session")!=status_->session || field<std::uint64_t>(*op.command,"expected_revision")!=status_->revision){op.state="superseded";op.detail="HWIO motor session or assignment changed";return;}
    Packet p;p.operation=op.kind.ends_with("-stop")?h::Operation::stop:h::Operation::reset_home;p.motor=unsigned(motor)%20;p.kind=psx::hwio::actuators::Kind(unsigned(motor)/20);p.revision=status_->revision;p.fault_revision=field<std::uint64_t>(*op.command,"fault_revision");
    if(p.operation==h::Operation::reset_home && p.fault_revision!=status_->motors[selected].fault_revision){op.state="superseded";op.detail="Motor fault changed";return;}
    action={send(p),0,status_->session,status_->revision,status_->motors[selected].generation,connection_,t};
    if(!action.request){op.state="failed";op.detail="Motor command queue unavailable";return;}op.detail="Motor command submitted";return;
  }
  if(action.connection!=connection_){op.state="indeterminate";op.detail="HWIO motor connection changed; command not replayed";return;}
  if(!available()){op.state="indeterminate";op.detail="HWIO motor connection lost; command not replayed";return;}
  if(action.session!=status_->session){op.state="indeterminate";op.detail="HWIO motor session changed; no replay";return;}
  if(action.revision!=status_->revision){op.state="indeterminate";op.detail="HWIO motor assignment changed; no replay";return;}
  Packet reply;
  if(action.request && receive(action.request,reply)){
    if(reply.result!=h::Result::ok){op.state="failed";op.detail=hwio_text(reply.message).empty()?"Motor request rejected":hwio_text(reply.message);return;}
    action.operation=reply.operation_id;
  }
  const auto& s=status_->motors[selected];
  if(action.generation!=s.generation){
    // A Reset may reopen its own channel. Only the acknowledged matching
    // operation can adopt that generation; unrelated replacement is uncertain.
    if(op.kind.ends_with("reset-home") && action.operation && s.operation_id==action.operation)action.generation=s.generation;
    else {op.state="indeterminate";op.detail="HWIO motor generation changed; no replay";return;}
  }
  if(action.operation && s.operation_id==action.operation && !(s.flags&h::reset_pending)){
    if(op.kind.ends_with("-stop")){if(!(s.flags&h::release_confirmed)){if(s.flags&h::major_fault){op.state="failed";op.detail=hwio_text(s.cause);return;}op.detail="Waiting for confirmed motor release";return;}}
    else if(s.flags&(h::major_fault|h::reset_failed)){op.state="failed";op.detail=hwio_text(s.cause);return;}
    op.state="observed";op.detail=hwio_text(s.cause);op.result_json="{\"drive_release_confirmed\":"+std::string(s.flags&h::release_confirmed?"true":"false")+",\"homed\":"+(s.flags&h::homing_required?"false":"true")+",\"physical_target_attained\":false}";return;
  }
  if(t-action.since>=(op.kind.ends_with("-stop")?5000:600000)){op.state="indeterminate";op.detail="Motor reset completion unavailable; no replay";}
}
std::string HwioMotors::json()const{
  std::string s="{\"available\":"+std::string(available()?"true":"false")+",\"reason\":"+quote(reason_)+",\"protocol_version\":"+std::to_string(status_?status_->version:0)+",\"motor_count\":"+std::to_string(status_?std::min(20u,wire_slots()):0)+",\"session\":"+std::to_string(status_?status_->session:0)+",\"revision\":"+std::to_string(status_?status_->revision:0)+",\"motors\":[";
  if(status_)for(unsigned n=0;n<std::min(20u,wire_slots());++n){const auto& m=status_->motors[n];if(n)s+=',';s+="{\"motor\":"+std::to_string(n)+",\"generation\":"+std::to_string(m.generation)+",\"operation_id\":"+std::to_string(m.operation_id)+",\"fault_revision\":"+std::to_string(m.fault_revision)+",\"flags\":"+std::to_string(m.flags)+",\"phase\":"+std::to_string(unsigned(m.phase))+",\"cause\":"+quote(hwio_text(m.cause))+"}";}
  return s+"]}";
}
std::uint64_t HwioMotors::send(psx::hwio::motors::v2::Packet p){return send(observed_legacy(p));}
bool HwioMotors::receive(std::uint64_t id,psx::hwio::motors::v2::Packet& p){Packet value;if(!receive(id,value))return false;p={};std::memcpy(&p,&value,offsetof(h::Packet,motors));p.signature=0x32544d48;p.version=2;p.bytes=sizeof(p);for(unsigned n=0;n<20;++n)std::memcpy(&p.motors[n],&value.motors[n],sizeof(h::Status));p.host=value.host;p.message=value.message;return true;}
std::string HwioMotors::actuator_json()const{
  std::string out="{\"available\":"+std::string(available()?"true":"false")+",\"reason\":"+quote(reason_)+",\"protocol_version\":"+std::to_string(status_?status_->version:0)+",\"session\":"+std::to_string(status_?status_->session:0)+",\"revision\":"+std::to_string(status_?status_->revision:0)+",\"actuators\":[";
  if(status_)for(unsigned n=0;n<wire_slots();++n){const auto& s=status_->motors[n];if(n)out+=',';out+="{\"kind\":"+quote(n<20?"motor":n<40?"servo":"stepper")+",\"index\":"+std::to_string(n%20)+",\"generation\":"+std::to_string(s.generation)+",\"fault_revision\":"+std::to_string(s.fault_revision)+",\"operation_id\":"+std::to_string(s.operation_id)+",\"flags\":"+std::to_string(s.flags)+",\"phase\":"+std::to_string(unsigned(s.phase))+",\"reference_quality\":"+std::to_string(unsigned(s.reference))+",\"position\":"+std::to_string(s.position)+",\"target\":"+std::to_string(s.target)+",\"cause\":"+quote(hwio_text(s.cause))+"}";}
  return out+"]}";
}

}
