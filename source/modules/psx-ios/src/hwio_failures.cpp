#include <ios/hwio_failures.hpp>
#include <ios/airport.hpp>
#include <ios/hwio_peer.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cctype>
namespace ios {
namespace {
namespace h=psx::hwio::failures;
std::uint64_t utc_now() {return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
}
HwioFailures::HwioFailures(std::wstring endpoint,psx::non_normal::ProcessIdentity expected):endpoint_(std::move(endpoint)),explicit_peer_(expected.pid?expected:psx::non_normal::current_process()) {read_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);write_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
HwioFailures::~HwioFailures(){disconnected();if(read_.hEvent)CloseHandle(read_.hEvent);if(write_.hEvent)CloseHandle(write_.hEvent);}
void HwioFailures::disconnected() {
  if(pipe_!=INVALID_HANDLE_VALUE) {CancelIoEx(pipe_,nullptr);DWORD n{};if(reading_)GetOverlappedResult(pipe_,&read_,&n,TRUE);if(writing_)GetOverlappedResult(pipe_,&write_,&n,TRUE);CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}
  reading_=writing_=awaiting_=false;pending_.clear();replies_.clear();status_.reset();source_valid_=false;source_sequence_=source_generation_=0;reason_="HWIO unavailable";
}
bool HwioFailures::connect() {
  pipe_=open_hwio_pipe(h::pipe_prefix,endpoint_,explicit_peer_);
  if(pipe_==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_ACCESS_DENIED)reason_="HWIO pipe identity rejected";return false;}
  h::Packet p;p.operation=h::Operation::status;send(p);reason_="Waiting for HWIO status";return true;
}
std::uint64_t HwioFailures::send(Packet p) {
  if(pipe_==INVALID_HANDLE_VALUE||pending_.size()>=32||next_==UINT64_MAX)return 0;
  p.request_id=next_++;if(status_)p.session=status_->session;pending_.push_back(p);return p.request_id;
}
bool HwioFailures::receive(std::uint64_t id,Packet &p) {
  auto i=std::find_if(replies_.begin(),replies_.end(),[&](const auto &r){return r.request_id==id;});if(i==replies_.end())return false;
  p=*i;replies_.erase(i);return true;
}
bool HwioFailures::available() const {return pipe_!=INVALID_HANDLE_VALUE&&status_&&(status_->data.flags&(h::ready|h::bound))==(h::ready|h::bound);}
void HwioFailures::accept(const Packet &p) {
  if(p.signature!=h::magic||p.protocol!=h::version||p.bytes!=h::packet_bytes||p.request_id!=outgoing_.request_id||p.operation!=outgoing_.operation||unsigned(p.result)>unsigned(h::Result::random_unavailable)) {disconnected();reason_="Malformed HWIO reply; no replay";return;}
  if(((p.data.flags&(h::ready|h::bound)) && !p.session) || (status_ && status_->session && p.session!=status_->session)) {disconnected();reason_="HWIO PSX session changed; no replay";return;}
  if((p.data.flags&h::ready) && (hwio_endpoint(hwio_text(p.data.host))!=hwio_endpoint(host_) || p.data.port!=port_)) {disconnected();reason_="HWIO is connected to a different PSX endpoint";return;}
  awaiting_=false;status_=p;reason_=hwio_text(p.data.message);
  if(p.operation==h::Operation::status && !(p.data.flags&h::bound)) {
    if(!(p.data.flags&h::ready)){reason_="HWIO PSX connection is not ready";return;}
    h::Packet bind;bind.operation=h::Operation::bind;bind.session=p.session;bind.data.port=port_;std::copy(host_.begin(),host_.end(),bind.data.host.begin());send(bind);
  }
  if(p.operation!=h::Operation::status&&p.operation!=h::Operation::bind&&p.operation!=h::Operation::altitude_sample) {
    if(replies_.size()>=32)replies_.pop_front();replies_.push_back(p);
  }
}
void HwioFailures::io(Tick t) {
  if(pipe_==INVALID_HANDLE_VALUE)return;
  DWORD n{};
  if(writing_) {
    if(GetOverlappedResult(pipe_,&write_,&n,FALSE)) {writing_=false;if(n!=sizeof(Packet)){disconnected();return;}}
    else if(GetLastError()!=ERROR_IO_INCOMPLETE){disconnected();return;}
  }
  if(reading_) {
    if(GetOverlappedResult(pipe_,&read_,&n,FALSE)) {reading_=false;if(n!=sizeof(Packet)){disconnected();return;}accept(incoming_);}
    else if(GetLastError()!=ERROR_IO_INCOMPLETE){disconnected();return;}
  }
  if(pipe_==INVALID_HANDLE_VALUE)return;
  if(awaiting_&&t>=reply_deadline_){disconnected();reason_="HWIO reply unavailable; no replay";return;}
  if(!awaiting_&&!writing_&&!pending_.empty()) {
    outgoing_=pending_.front();pending_.pop_front();awaiting_=true;reply_deadline_=t+2000;
    ResetEvent(write_.hEvent);write_.Internal=write_.InternalHigh=0;write_.Offset=write_.OffsetHigh=0;
    if(WriteFile(pipe_,&outgoing_,sizeof(Packet),&n,&write_)) {if(n!=sizeof(Packet)){disconnected();return;}}
    else if(GetLastError()==ERROR_IO_PENDING)writing_=true;else{disconnected();return;}
  }
  if(awaiting_&&!reading_) {
    ResetEvent(read_.hEvent);read_.Internal=read_.InternalHigh=0;read_.Offset=read_.OffsetHigh=0;
    if(ReadFile(pipe_,&incoming_,sizeof(Packet),&n,&read_)) {if(n!=sizeof(Packet)){disconnected();return;}accept(incoming_);}
    else if(GetLastError()==ERROR_IO_PENDING)reading_=true;else disconnected();
  }
}
void HwioFailures::poll(Tick t,bool ready,std::string host,std::uint16_t port,const std::optional<psx::instructor::InstructorSample>&sample) {
  if(!ready){disconnected();return;}
  if(host!=host_||port!=port_){disconnected();host_=std::move(host);port_=port;connect_at_=0;}
  if(host_.size()>=128){reason_="PSX endpoint exceeds HWIO bounds";return;}
  if(pipe_==INVALID_HANDLE_VALUE&&t>=connect_at_){connect_at_=t+1000;connect();status_at_=t+500;}
  io(t);
  if(available()) {
    const auto time=utc_now();const bool valid=sample&&sample->corrected_altitude_ft&&sample->loading==false&&sample->sampled_unix_ms<=time&&time-sample->sampled_unix_ms<=h::sample_max_age_ms;
    if((valid&&(sample->sequence!=source_sequence_||sample->lifecycle_epoch!=source_generation_))||valid!=source_valid_) {
      h::Packet p;p.operation=h::Operation::altitude_sample;
      if(sample){p.data.source_pid=sample->pid;p.data.source_start_ms=sample->process_start_unix_ms;p.data.source_generation=sample->lifecycle_epoch;p.data.revision=sample->sequence;p.data.sampled_unix_ms=std::int64_t(sample->sampled_unix_ms);}
      if(valid){p.data.flags=h::sample_valid;p.data.value=*sample->corrected_altitude_ft;}
      if(send(p)){source_valid_=valid;source_sequence_=sample?sample->sequence:0;source_generation_=sample?sample->lifecycle_epoch:0;}
    }
  }
  if(pipe_!=INVALID_HANDLE_VALUE&&t>=status_at_){h::Packet p;p.operation=h::Operation::status;send(p);status_at_=t+500;}
}
std::string HwioFailures::json() const {
  auto flags=status_?status_->data.flags:0;
  std::string s="{\"available\":"+std::string(available()?"true":"false")+",\"reason\":"+quote(reason_)+",\"revision\":"+std::to_string(status_?status_->data.revision:0)+
    ",\"axis_mask\":"+std::to_string(status_?status_->data.axis_mask:0)+",\"trim_direction\":"+std::to_string(status_?status_->data.trim:0)+
    ",\"ias_available\":"+(flags&h::ias_available?"true":"false")+",\"altitude_available\":"+(flags&h::altitude_available?"true":"false")+",\"altitude_sampling_ms\":1000,\"arms\":[";
  if(status_)for(unsigned i=0;i<std::min(status_->data.arm_count,h::max_arms);++i){const auto &a=status_->data.arms[i];if(i)s+=',';s+="{\"id\":"+std::to_string(a.id)+",\"axis_mask\":"+std::to_string(a.axis_mask)+",\"trim\":"+(a.trim?"true":"false")+",\"criterion\":"+quote(a.trigger==unsigned(h::Trigger::ias)?"ias":"altitude")+",\"value\":"+number(a.threshold)+"}";}
  return s+"]}";
}
}
