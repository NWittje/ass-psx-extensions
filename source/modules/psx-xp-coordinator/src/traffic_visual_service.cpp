#include <psx/sync/worker_io.hpp>
#include <optional>
#include "coordinator/traffic_visual.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/status.hpp"
#include "readiness_completion.hpp"
#include "traffic_ingress_batch.hpp"
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace coordinator {
namespace w=visual_wire;
namespace {
std::uint64_t birth(HANDLE process) noexcept {
 FILETIME c{},e{},k{},u{};
 return GetProcessTimes(process,&c,&e,&k,&u)?std::uint64_t(c.dwLowDateTime)|(std::uint64_t(c.dwHighDateTime)<<32):0;
}
std::wstring sid(HANDLE process) {
 HANDLE raw{};if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return {};
 WinHandle token(raw);alignas(TOKEN_USER) std::array<std::uint8_t,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> bytes{};
 DWORD count{};if(!GetTokenInformation(token.get(),TokenUser,bytes.data(),static_cast<DWORD>(bytes.size()),&count))return {};
 LPWSTR text{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))return {};
 std::wstring result(text);LocalFree(text);return result;
}
struct Operation {
 WinHandle event{make_stop_event()};OVERLAPPED value{};bool pending{};HANDLE target{};
 void prepare(HANDLE pipe) noexcept {target=pipe;ResetEvent(event.get());value={};value.hEvent=event.get();}
 void cancel() noexcept {if(pending){psx::sync::io_cancel(target,&value);DWORD count{};psx::sync::io_result(target,&value,&count,TRUE);pending=false;}}
 ~Operation(){cancel();} // The pipe and buffer remain alive until retirement completes.
};
void sequence(w::Frame& frame,std::uint32_t value) noexcept {
 for(unsigned n=0;n<4;++n)frame.bytes[12+n]=static_cast<std::uint8_t>(value>>(n*8));
}
void envelope(w::Frame& frame,const w::Header& h,const std::uint8_t* bytes,std::size_t count) noexcept {
 frame.size=w::header_size+count;frame.bytes[0]='P';frame.bytes[1]='X';frame.bytes[2]='V';frame.bytes[3]='1';frame.bytes[4]=1;frame.bytes[5]=0;
 for(unsigned n=0;n<2;++n)frame.bytes[6+n]=static_cast<std::uint8_t>(static_cast<unsigned>(h.kind)>>(n*8));
 for(unsigned n=0;n<4;++n)frame.bytes[8+n]=static_cast<std::uint8_t>(h.payload_size>>(n*8));
 sequence(frame,h.sequence);std::copy(bytes,bytes+count,frame.bytes.begin()+w::header_size);
}
bool header(const w::Frame& f,w::Header& h) noexcept {
 return f.size>=w::header_size&&f.size<=w::max_frame&&w::read_header(f.bytes.data(),w::header_size,h)&&f.size==w::header_size+h.payload_size;
}
}
psx::motion::InstanceId traffic_visual_owner() {
 psx::motion::InstanceId value{};
 if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&value),sizeof(value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0||!psx::motion::valid(value))throw std::runtime_error("Traffic visual owner identity");
 return value;
}
std::wstring traffic_visual_plugin_pipe_name(ProcessIdentity process) {
 const auto user=sid(GetCurrentProcess());if(user.empty())throw_win32_error("Traffic visual SID");
 return L"\\\\.\\pipe\\psx-ios.traffic.plugin.v1."+user+L"."+std::to_wstring(process.pid)+L"."+std::to_wstring(process.creation_filetime);
}
struct TrafficVisualService::Impl {
 mutable std::mutex mutex;ProcessIdentity expected{};std::wstring fixture;
 std::uint64_t epoch{},frequency{},live_connection{};psx::motion::InstanceId owner{};
 WinHandle cancel{make_stop_event()},wake{make_stop_event()},ready{make_stop_event()};std::thread worker;
 std::atomic<bool> finished{true};std::atomic<std::uint64_t> drops{},coalesces{},connections{};
 std::array<TrafficVisualEvent,256> events{};std::size_t head{},size{},detail_count{};
 std::array<w::Frame,16> controls{};std::size_t control_head{},control_size{};
 w::Frame latest{},capture{};bool has_latest{},has_capture{};
 void clear_output() noexcept {control_head=control_size=0;has_latest=has_capture=false;latest.size=capture.size=0;}
 bool active(ProcessIdentity process,std::uint64_t domain) {
  if(WaitForSingleObject(cancel.get(),0)==WAIT_OBJECT_0)return false;
  std::lock_guard<std::mutex> lock(mutex);return expected==process&&epoch==domain;
 }
 bool push(const TrafficVisualEvent& e) {
  std::lock_guard<std::mutex> lock(mutex);w::Header h;
  const bool detail=e.kind==TrafficVisualEventKind::frame&&header(e.frame,h)&&h.kind==w::Kind::detail;
  // Detail loss marks Capture incomplete, but cannot revoke a healthy traffic lease.
  // Ordered native/control/write-completion evidence is never silently coalesced.
  if(detail&&(detail_count==64||size>=events.size()-32)){++drops;return true;}
  if(size==events.size()){
   drops+=size+1;head=size=detail_count=0;clear_output();live_connection=0;++epoch;
   TrafficVisualEvent loss;loss.process=expected;loss.connection=e.connection;loss.received_qpc=e.received_qpc;loss.reason=w::Reason::overflow;loss.error=ERROR_BUFFER_OVERFLOW;
   events[size++]=loss;SetEvent(wake.get());SetEvent(ready.get());return false;
  }
  events[(head+size)%events.size()]=e;++size;if(detail)++detail_count;SetEvent(ready.get());return true;
 }
 void lost(ProcessIdentity process,std::uint64_t connection,DWORD error,w::Reason reason) {
  TrafficVisualEvent e;e.process=process;e.connection=connection;e.received_qpc=qpc_now();e.error=error;e.reason=reason;push(e);
 }
 struct Peer {
  Impl* self{};ProcessIdentity process{};std::uint64_t connection{};w::Hello hello{};
  bool seen{},announced{};std::int64_t last_input{};
  static bool receive(void* context,const w::Header& h,const std::uint8_t* bytes,std::size_t count) {
   auto& p=*static_cast<Peer*>(context);const auto now=qpc_now();
   if(!p.seen){
    if(h.kind!=w::Kind::hello||!w::decode(bytes,count,p.hello)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=p.self->frequency)return false;
    p.seen=true;p.last_input=now;return true;
   }
   if(!p.announced)return false;
   if(h.kind==w::Kind::heartbeat){w::Heartbeat v;if(!w::decode(bytes,count,v)||v.sender!=p.hello.instance||v.connection!=p.connection||v.generated_qpc>now)return false;}
   else if(h.kind==w::Kind::ack){w::Ack v;if(!w::decode(bytes,count,v)||v.plugin!=p.hello.instance||v.connection!=p.connection||v.generated_qpc>now)return false;}
   else if(h.kind==w::Kind::report){w::Report v;if(!w::decode(bytes,count,v)||v.plugin!=p.hello.instance||v.connection!=p.connection||v.generated_qpc>now)return false;}
   else if(h.kind==w::Kind::detail){w::Detail v;if(!w::decode(bytes,count,v)||v.plugin!=p.hello.instance||v.connection!=p.connection||v.generated_qpc>now)return false;}
   else return false;
   TrafficVisualEvent e;e.kind=TrafficVisualEventKind::frame;e.process=p.process;e.connection=p.connection;e.received_qpc=now;envelope(e.frame,h,bytes,count);
   p.last_input=now;return p.self->push(e);
  }
 };
 bool write_frame(HANDLE pipe,const w::Frame& frame,ProcessIdentity process,std::uint64_t domain,std::int64_t deadline=0) {
  Operation op;std::size_t offset{};auto progress=qpc_now();
  while(offset<frame.size&&active(process,domain)){
   const auto now=qpc_now();if(now-progress>=static_cast<std::int64_t>(frequency)||(deadline&&now>=deadline))return false;
   DWORD count{};op.prepare(pipe);
   if(psx::sync::io_write(pipe,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&count,&op.value)){if(!count)return false;offset+=count;progress=qpc_now();continue;}
   if(GetLastError()!=ERROR_IO_PENDING)return false;op.pending=true;
   while(active(process,domain)&&qpc_now()-progress<static_cast<std::int64_t>(frequency)&&(!deadline||qpc_now()<deadline)){
    if(!psx::sync::worker_poll(pipe))return false;
    HANDLE waits[]{cancel.get(),op.event.get(),wake.get()};const auto wait=WaitForMultipleObjects(3,waits,FALSE,20);
    if(wait==WAIT_OBJECT_0+1){const auto result=readiness_io::poll(pipe,op.value);if(result.pending)continue;op.pending=false;if(!result.success||!result.bytes)return false;offset+=result.bytes;progress=qpc_now();break;}
    if(wait==WAIT_OBJECT_0+2)ResetEvent(wake.get());if(wait==WAIT_FAILED)return false;
   }
   if(op.pending)return false;
  }
  return offset==frame.size;
 }
 bool output(w::Frame& f,std::int64_t now,std::int64_t& last_bundle) {
  std::lock_guard<std::mutex> lock(mutex);
  if(control_size){f=controls[control_head];control_head=(control_head+1)%controls.size();--control_size;return true;}
  if(has_capture){f=capture;has_capture=false;return true;}
  if(has_latest&&now-last_bundle>=static_cast<std::int64_t>((frequency+99)/100)){f=latest;has_latest=false;last_bundle=now;return true;}
  return false;
 }
 void run() {
  const bool remote=psx::sync::enabled();
  const auto user=remote?std::wstring{}:sid(GetCurrentProcess());if(!remote&&user.empty())throw_win32_error("Traffic visual user token");
  while(WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0){
   ProcessIdentity selected;std::wstring endpoint;std::uint64_t domain{};
   {std::lock_guard<std::mutex> lock(mutex);selected=expected;endpoint=fixture;domain=epoch;ResetEvent(wake.get());}
   if(!selected.pid){HANDLE waits[]{cancel.get(),wake.get()};WaitForMultipleObjects(2,waits,FALSE,250);continue;}
   WinHandle process(remote?nullptr:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,selected.pid));
   if(!remote&&(!process.valid()||birth(process.get())!=selected.creation_filetime||WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT||sid(process.get())!=user)){lost(selected,0,ERROR_ACCESS_DENIED,w::Reason::identity);WaitForSingleObject(cancel.get(),250);continue;}
   if(!remote&&endpoint.empty())endpoint=traffic_visual_plugin_pipe_name(selected);
   WinHandle pipe(remote?nullptr:CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
   if(!remote&&!pipe.valid()){lost(selected,0,GetLastError(),w::Reason::disconnected);WaitForSingleObject(cancel.get(),250);continue;}
   std::optional<psx::sync::WorkerIo> network;if(remote){const auto state=psx::sync::runtime()->status();if(!state.connected||state.peer.pid!=selected.pid||state.peer.process_birth!=selected.creation_filetime){WaitForSingleObject(cancel.get(),20);continue;}network.emplace(psx::sync::Family::traffic);}
   ULONG pid=remote?selected.pid:0;
   if((!remote&&(!GetNamedPipeServerProcessId(pipe.get(),&pid)||pid!=selected.pid||birth(process.get())!=selected.creation_filetime))||!active(selected,domain)){lost(selected,0,ERROR_ACCESS_DENIED,w::Reason::identity);WaitForSingleObject(cancel.get(),250);continue;}
   if(connections==UINT64_MAX){lost(selected,0,ERROR_ARITHMETIC_OVERFLOW,w::Reason::overflow);break;}
   Peer peer;peer.self=this;peer.process=selected;peer.connection=++connections;peer.last_input=qpc_now();
   {std::lock_guard<std::mutex> lock(mutex);clear_output();live_connection=0;}
   const auto started=peer.last_input;std::int64_t last_heartbeat{},last_bundle{};std::uint32_t wire_sequence{};DWORD failure{};
   w::Framer framer;std::array<std::uint8_t,4096> input{};Operation read;
   while(active(selected,domain)&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){
    if(!psx::sync::worker_poll(pipe.get()))break;
    const auto now=qpc_now();if((!peer.announced&&now-started>=static_cast<std::int64_t>(frequency))||(peer.announced&&now-peer.last_input>=static_cast<std::int64_t>(frequency*5))){failure=ERROR_TIMEOUT;break;}
    if(peer.seen&&(!peer.announced||now-last_heartbeat>=static_cast<std::int64_t>(frequency/4))){
     if(wire_sequence==UINT32_MAX){failure=ERROR_ARITHMETIC_OVERFLOW;break;}
     w::Frame f;const auto clock=qpc_now();w::Heartbeat heartbeat{owner,peer.connection,clock,w::supported,w::Reason::none};
     if(!w::encode(heartbeat,++wire_sequence,f)||!write_frame(pipe.get(),f,selected,domain,peer.announced?0:started+static_cast<std::int64_t>(frequency))){failure=ERROR_WRITE_FAULT;break;}
     last_heartbeat=qpc_now();
     if(!peer.announced){
      // Establish observing peer ownership before Claim/Capture, even without Follow.
      {std::lock_guard<std::mutex> lock(mutex);live_connection=peer.connection;}
      TrafficVisualEvent e;e.kind=TrafficVisualEventKind::hello;e.process=selected;e.connection=peer.connection;e.received_qpc=qpc_now();e.hello=peer.hello;
      if(!push(e)){failure=ERROR_BUFFER_OVERFLOW;break;}peer.announced=true;
     }
    }
    if(peer.announced){
     for(unsigned batch=0;batch<1;++batch){w::Frame f;if(!output(f,qpc_now(),last_bundle))break;
      if(wire_sequence==UINT32_MAX){failure=ERROR_ARITHMETIC_OVERFLOW;break;}sequence(f,++wire_sequence);
      if(!write_frame(pipe.get(),f,selected,domain)){failure=ERROR_WRITE_FAULT;break;}
      // Publish actual successful completion before servicing any buffered response.
      TrafficVisualEvent e;e.kind=TrafficVisualEventKind::sent;e.process=selected;e.connection=peer.connection;e.received_qpc=qpc_now();e.frame=f;
      w::Header sent_header;if(header(f,sent_header)&&sent_header.kind==w::Kind::bundle)last_bundle=e.received_qpc;
      if(!push(e)){failure=ERROR_BUFFER_OVERFLOW;break;}
     }
     if(failure)break;
    }
    DWORD count{};
    if(remote){
     static_assert(8*w::max_frame<=16384);
     const auto receive=[&](DWORD& received){
      if(read.pending){
       if(WaitForSingleObject(read.event.get(),0)!=WAIT_OBJECT_0)return detail::TrafficReadResult::pending;
       const auto result=readiness_io::poll(pipe.get(),read.value);if(result.pending)return detail::TrafficReadResult::pending;
       read.pending=false;received=result.bytes;
       if(!result.success||!received){failure=result.success?ERROR_BROKEN_PIPE:result.error;return detail::TrafficReadResult::failed;}
      }else{
       read.prepare(pipe.get());
       if(!psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&received,&read.value)){
        const auto error=GetLastError();if(error==ERROR_IO_PENDING){read.pending=true;return detail::TrafficReadResult::pending;}
        failure=error;return detail::TrafficReadResult::failed;
       }
      }
      return detail::TrafficReadResult::complete;
     };
     if(!detail::drain_remote_traffic_ingress(receive,[&](DWORD received){
      if(!received||received>w::max_frame||!framer.feed(input.data(),received,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;return detail::TrafficConsumeResult::failed;}
      return peer.seen&&!peer.announced?detail::TrafficConsumeResult::yield:detail::TrafficConsumeResult::consumed;
     }))break;
     if(peer.seen&&!peer.announced)continue;
    }else{
     if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){
      const auto result=readiness_io::poll(pipe.get(),read.value);if(result.pending)continue;read.pending=false;count=result.bytes;
      if(!result.success||!count){failure=result.success?ERROR_BROKEN_PIPE:result.error;break;}
      if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}if(peer.seen&&!peer.announced)continue;
     }
     if(!read.pending){read.prepare(pipe.get());
      if(psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value)){if(!count||!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}continue;}
      if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else {failure=GetLastError();break;}
     }
    }
    HANDLE waits[]{cancel.get(),read.event.get(),wake.get()};const auto wait=WaitForMultipleObjects(3,waits,FALSE,5);
    if(wait==WAIT_OBJECT_0+2)ResetEvent(wake.get());if(wait==WAIT_FAILED){failure=GetLastError();break;}
   }
   read.cancel();{std::lock_guard<std::mutex> lock(mutex);clear_output();live_connection=0;}
   lost(selected,peer.connection,failure?failure:ERROR_OPERATION_ABORTED,failure==ERROR_TIMEOUT?w::Reason::timeout:failure==ERROR_INVALID_DATA?w::Reason::protocol:w::Reason::disconnected);
   WaitForSingleObject(cancel.get(),250);
  }
 }
};
TrafficVisualService::TrafficVisualService():impl_(std::make_unique<Impl>()){}
TrafficVisualService::~TrafficVisualService(){request_stop();join();}
void TrafficVisualService::start(std::uint64_t frequency,psx::motion::InstanceId owner) {
 if(impl_->worker.joinable())return;if(!frequency||frequency>1000000000000ULL||!psx::motion::valid(owner))throw std::invalid_argument("Traffic visual service configuration");
 {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->head=impl_->size=impl_->detail_count=0;impl_->clear_output();impl_->live_connection=0;ResetEvent(impl_->ready.get());}
 impl_->frequency=frequency;impl_->owner=owner;ResetEvent(impl_->cancel.get());impl_->finished=false;
 impl_->worker=std::thread([p=impl_.get()]{try{p->run();}catch(...){p->lost({},0,ERROR_UNHANDLED_EXCEPTION,w::Reason::native_fault);}p->finished=true;});
}
void TrafficVisualService::associate(ProcessIdentity expected) {
 std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->expected==expected)return;
 impl_->expected=expected;++impl_->epoch;impl_->clear_output();impl_->live_connection=0;SetEvent(impl_->wake.get());
}
void TrafficVisualService::request_stop() noexcept {SetEvent(impl_->cancel.get());}
void TrafficVisualService::join(){if(impl_->worker.joinable())impl_->worker.join();}
bool TrafficVisualService::done() const noexcept {return impl_->finished;}
bool TrafficVisualService::pop(TrafficVisualEvent& e) {
 std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->size)return false;
 e=impl_->events[impl_->head];w::Header h;if(e.kind==TrafficVisualEventKind::frame&&header(e.frame,h)&&h.kind==w::Kind::detail)--impl_->detail_count;
 impl_->head=(impl_->head+1)%impl_->events.size();--impl_->size;if(!impl_->size)ResetEvent(impl_->ready.get());return true;
}
bool TrafficVisualService::send(const w::Frame& f) {
 w::Header h;if(!header(f,h))return false;const auto* bytes=f.bytes.data()+w::header_size;
 w::Claim control;w::Bundle bundle;w::Capture capture;std::uint64_t connection{};
 if(h.kind==w::Kind::claim){if(!w::decode(bytes,h.payload_size,control)||control.owner!=impl_->owner)return false;connection=control.connection;}
 else if(h.kind==w::Kind::bundle){if(!w::decode(bytes,h.payload_size,bundle)||bundle.owner!=impl_->owner)return false;connection=bundle.connection;}
 else if(h.kind==w::Kind::capture){if(!w::decode(bytes,h.payload_size,capture))return false;connection=capture.connection;}
 else return false;
 std::lock_guard<std::mutex> lock(impl_->mutex);if(!connection||connection!=impl_->live_connection||!impl_->expected.pid)return false;
 if(h.kind==w::Kind::claim){
  if(control.action==w::Action::release){
   std::array<w::Frame,16> retained{};std::size_t count{};
   for(std::size_t i=0;i<impl_->control_size;++i){const auto& entry=impl_->controls[(impl_->control_head+i)%impl_->controls.size()];w::Header eh;w::Claim c;if(header(entry,eh)&&w::decode(entry.bytes.data()+w::header_size,eh.payload_size,c)&&(c.lease!=control.lease||c.action==w::Action::release))retained[count++]=entry;}
   impl_->controls=retained;impl_->control_head=0;impl_->control_size=count;
   w::Header bh;w::Bundle old;if(impl_->has_latest&&header(impl_->latest,bh)&&w::decode(impl_->latest.bytes.data()+w::header_size,bh.payload_size,old)&&old.lease==control.lease)impl_->has_latest=false;
  }
  if(impl_->control_size==impl_->controls.size()){
   ++impl_->drops;++impl_->epoch;impl_->clear_output();impl_->live_connection=0;
   TrafficVisualEvent e;e.process=impl_->expected;e.connection=connection;e.received_qpc=qpc_now();e.reason=w::Reason::overflow;e.error=ERROR_BUFFER_OVERFLOW;
   if(impl_->size==impl_->events.size()){impl_->drops+=impl_->size;impl_->head=impl_->size=impl_->detail_count=0;}
   impl_->events[(impl_->head+impl_->size)%impl_->events.size()]=e;++impl_->size;SetEvent(impl_->ready.get());SetEvent(impl_->wake.get());return false;
  }
  impl_->controls[(impl_->control_head+impl_->control_size)%impl_->controls.size()]=f;++impl_->control_size;
 }else if(h.kind==w::Kind::bundle){if(impl_->has_latest)++impl_->coalesces;impl_->latest=f;impl_->has_latest=true;}
 else {if(impl_->has_capture)++impl_->coalesces;impl_->capture=f;impl_->has_capture=true;}
 SetEvent(impl_->wake.get());return true;
}
void TrafficVisualService::fault(std::uint32_t error) noexcept {
 ProcessIdentity process;std::uint64_t connection{};{std::lock_guard<std::mutex> lock(impl_->mutex);process=impl_->expected;connection=impl_->live_connection;++impl_->epoch;impl_->clear_output();impl_->live_connection=0;SetEvent(impl_->wake.get());}
 impl_->lost(process,connection,error,w::Reason::protocol);
}
std::uintptr_t TrafficVisualService::ready_event() const noexcept {return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t TrafficVisualService::dropped() const noexcept {return impl_->drops.load();}
std::uint64_t TrafficVisualService::coalesced() const noexcept {return impl_->coalesces.load();}
void TrafficVisualService::fixture_name(std::wstring name) {
 std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->worker.joinable())throw std::logic_error("Traffic visual fixture endpoint must precede start");impl_->fixture=std::move(name);
}
}
