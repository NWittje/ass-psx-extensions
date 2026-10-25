#include "coordinator/traffic.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/status.hpp"
#include "readiness_completion.hpp"
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
namespace coordinator {
namespace w=traffic_wire;
namespace {
std::uint64_t birth(HANDLE h){FILETIME a{},b{},c{},d{};return GetProcessTimes(h,&a,&b,&c,&d)?std::uint64_t(a.dwLowDateTime)|(std::uint64_t(a.dwHighDateTime)<<32):0;}
std::wstring sid(HANDLE process){HANDLE raw{};if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return {};WinHandle token(raw);DWORD n{};GetTokenInformation(token.get(),TokenUser,nullptr,0,&n);std::vector<std::uint8_t> data(n);if(!n||!GetTokenInformation(token.get(),TokenUser,data.data(),n,&n))return {};LPWSTR str{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&str))return {};std::wstring result(str);LocalFree(str);return result;}
struct Operation {
 WinHandle event{make_stop_event()};OVERLAPPED value{};bool pending{};HANDLE target{};
 void prepare(HANDLE handle){target=handle;ResetEvent(event.get());value={};value.hEvent=event.get();}
 void cancel() noexcept {if(pending){CancelIoEx(target,&value);DWORD n{};GetOverlappedResult(target,&value,&n,TRUE);pending=false;}}
 ~Operation(){cancel();} // Pipe and buffers outlive this retirement.
};
void header(w::Frame& frame,const w::Header& h){frame.size=w::header_size+h.payload_size;frame.bytes[0]='P';frame.bytes[1]='X';frame.bytes[2]='F';frame.bytes[3]='1';frame.bytes[4]=1;frame.bytes[5]=0;for(unsigned n=0;n<2;++n)frame.bytes[6+n]=static_cast<std::uint8_t>(static_cast<unsigned>(h.kind)>>(n*8));for(unsigned n=0;n<4;++n){frame.bytes[8+n]=static_cast<std::uint8_t>(h.payload_size>>(n*8));frame.bytes[12+n]=static_cast<std::uint8_t>(h.sequence>>(n*8));}}
}
std::wstring traffic_exporter_pipe_name(ProcessIdentity p){const auto user=sid(GetCurrentProcess());if(user.empty())throw_win32_error("Traffic SID");return L"\\\\.\\pipe\\psx-ios.traffic.exporter.v1."+user+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
struct TrafficService::Impl {
 mutable std::mutex mutex;ProcessIdentity expected{};std::wstring name;std::uint64_t epoch{},selection{},frequency{};
 WinHandle cancel{make_stop_event()},wake{make_stop_event()},ready{make_stop_event()};std::thread worker;
 std::atomic<bool> finished{true};std::atomic<std::uint64_t> drops{},connections{};
 std::array<TrafficEvent,256> queue{};std::size_t head{},size{};
 HANDLE fixture_subscribe_release{};
 bool active(ProcessIdentity p,std::uint64_t domain){if(WaitForSingleObject(cancel.get(),0)==WAIT_OBJECT_0)return false;std::lock_guard<std::mutex> lock(mutex);return expected==p&&epoch==domain;}
 bool push(const TrafficEvent& e){std::lock_guard<std::mutex> lock(mutex);const bool sample=e.kind==TrafficEventKind::frame&&e.frame.size>w::header_size&&e.frame.bytes[6]==static_cast<unsigned>(w::Kind::sample);
  // Sixteen slots remain available to ordered protocol/loss records.
  if(size==queue.size()||(sample&&size>=queue.size()-16)){drops+=size+1;head=size=0;TrafficEvent loss;loss.process=expected;loss.selection_epoch=selection;loss.error=ERROR_BUFFER_OVERFLOW;loss.reason=w::Reason::overflow;loss.received_qpc=e.received_qpc;queue[size++]=loss;++epoch;SetEvent(wake.get());SetEvent(ready.get());return false;}
  queue[(head+size)%queue.size()]=e;++size;SetEvent(ready.get());return true;
 }
 void lost(ProcessIdentity p,std::uint64_t connection,std::uint64_t selected,DWORD error,w::Reason reason){TrafficEvent e;e.process=p;e.connection_id=connection;e.selection_epoch=selected;e.error=error;e.reason=reason;e.received_qpc=qpc_now();push(e);}
 struct Peer {
  Impl* self{};ProcessIdentity process{};w::Hello hello{};bool hello_seen{},subscribed{};
  std::uint64_t connection{},selection{};std::int64_t floor{},last_input{};std::uint32_t capabilities{};
  static bool receive(void* context,const w::Header& h,const std::uint8_t* data,std::size_t n){auto& p=*static_cast<Peer*>(context);auto& self=*p.self;const auto now=qpc_now();
   if(!p.hello_seen){if(h.kind!=w::Kind::hello||!w::decode(data,n,p.hello)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=self.frequency)return false;p.hello_seen=true;p.capabilities=p.hello.capabilities;p.last_input=now;return true;}
   if(!p.subscribed)return false;
   if(h.kind==w::Kind::sample){w::Sample v;if(!w::decode(data,n,v)||v.producer!=p.hello.instance||v.connection_id!=p.connection||v.selection_epoch!=p.selection||v.snapshot.header.process!=p.process.pid||v.snapshot.header.process_created!=p.process.creation_filetime||v.snapshot.header.frequency!=self.frequency||v.snapshot.header.finished>static_cast<std::uint64_t>(now))return false;}
   else if(h.kind==w::Kind::heartbeat||h.kind==w::Kind::status){w::Heartbeat v;if(h.kind==w::Kind::heartbeat){if(!w::decode(data,n,v))return false;}else {w::Status status;if(!w::decode(data,n,status))return false;v={status.producer,status.connection_id,status.selection_epoch,status.generated_qpc,status.capabilities,status.reason};}
    if(v.producer!=p.hello.instance||v.connection_id!=p.connection||v.selection_epoch!=p.selection||v.generated_qpc<p.floor||v.generated_qpc>now)return false;p.capabilities=v.capabilities;
   }else return false;
   TrafficEvent e;e.kind=TrafficEventKind::frame;e.process=p.process;e.connection_id=p.connection;e.selection_epoch=p.selection;e.subscribe_floor_qpc=p.floor;e.received_qpc=now;header(e.frame,h);std::copy(data,data+n,e.frame.bytes.begin()+w::header_size);p.last_input=now;return self.push(e);
  }
 };
 bool write_frame(HANDLE pipe,const w::Frame& frame,ProcessIdentity p,std::uint64_t domain,std::int64_t handshake_deadline=0){Operation op;std::size_t offset{};auto progress=qpc_now();
  while(offset<frame.size&&active(p,domain)){
   const auto now=qpc_now();if(now-progress>=static_cast<std::int64_t>(frequency)||(handshake_deadline&&now>=handshake_deadline))return false;
   DWORD n{};op.prepare(pipe);if(WriteFile(pipe,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&n,&op.value)){if(!n)return false;offset+=n;progress=qpc_now();continue;}
   if(GetLastError()!=ERROR_IO_PENDING)return false;op.pending=true;
   while(active(p,domain)&&qpc_now()-progress<static_cast<std::int64_t>(frequency)&&(!handshake_deadline||qpc_now()<handshake_deadline)){
    HANDLE waits[]{cancel.get(),op.event.get(),wake.get()};const auto result=WaitForMultipleObjects(3,waits,FALSE,20);
    if(result==WAIT_OBJECT_0+1){const auto completion=readiness_io::poll(pipe,op.value);if(completion.pending)continue;op.pending=false;if(!completion.success||!completion.bytes)return false;offset+=completion.bytes;progress=qpc_now();break;}
    if(result==WAIT_OBJECT_0+2)ResetEvent(wake.get());if(result==WAIT_FAILED)return false;
   }
   if(op.pending)return false;
  }return offset==frame.size;
 }
 void run(){const auto user=sid(GetCurrentProcess());if(user.empty())throw_win32_error("Traffic user token");const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+user+L")";
  while(WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0){ProcessIdentity selected;std::wstring endpoint;std::uint64_t domain{},selected_epoch{};
   {std::lock_guard<std::mutex> lock(mutex);selected=expected;endpoint=name;domain=epoch;selected_epoch=selection;ResetEvent(wake.get());}
   if(!selected.pid){HANDLE waits[]{cancel.get(),wake.get()};WaitForMultipleObjects(2,waits,FALSE,250);continue;}
   if(endpoint.empty())endpoint=traffic_exporter_pipe_name(selected);
   PSECURITY_DESCRIPTOR raw{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){lost(selected,0,selected_epoch,GetLastError(),w::Reason::identity);WaitForSingleObject(cancel.get(),250);continue;}
   SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};WinHandle pipe(CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&security));LocalFree(raw);
   if(!pipe.valid()){lost(selected,0,selected_epoch,GetLastError(),w::Reason::disconnected);WaitForSingleObject(cancel.get(),250);continue;}
   Operation attachment;attachment.prepare(pipe.get());bool attached=ConnectNamedPipe(pipe.get(),&attachment.value)!=FALSE;
   if(!attached){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;else if(error==ERROR_IO_PENDING){attachment.pending=true;while(active(selected,domain)){HANDLE waits[]{cancel.get(),attachment.event.get(),wake.get()};const auto result=WaitForMultipleObjects(3,waits,FALSE,20);if(result==WAIT_OBJECT_0+1){const auto completion=readiness_io::poll(pipe.get(),attachment.value);if(completion.pending)continue;attached=completion.success;attachment.pending=false;break;}if(result==WAIT_OBJECT_0+2)ResetEvent(wake.get());if(result==WAIT_FAILED)break;}}}
   attachment.cancel();if(!attached)continue;
   ULONG pid{};WinHandle process_handle;if(GetNamedPipeClientProcessId(pipe.get(),&pid))process_handle.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));const ProcessIdentity actual{pid,process_handle.valid()?birth(process_handle.get()):0};
   if(!process_handle.valid()||actual!=selected||sid(process_handle.get())!=user||!active(actual,domain)){lost(selected,0,selected_epoch,ERROR_ACCESS_DENIED,w::Reason::identity);DisconnectNamedPipe(pipe.get());WaitForSingleObject(cancel.get(),250);continue;}
   if(connections==UINT64_MAX){lost(actual,0,selected_epoch,ERROR_ARITHMETIC_OVERFLOW,w::Reason::protocol);break;}
   Peer peer;peer.self=this;peer.process=actual;peer.connection=++connections;peer.selection=selected_epoch;peer.last_input=qpc_now();const auto started=peer.last_input;
   w::Framer framer;std::array<std::uint8_t,4096> input{};Operation read;std::uint32_t sequence{};std::int64_t last_output{};DWORD failure{};
   while(active(actual,domain)&&WaitForSingleObject(process_handle.get(),0)==WAIT_TIMEOUT){const auto now=qpc_now();if((!peer.subscribed&&now-started>=static_cast<std::int64_t>(frequency))||(peer.subscribed&&now-peer.last_input>=static_cast<std::int64_t>(frequency*5))){failure=ERROR_TIMEOUT;break;}
    if(peer.hello_seen&&!peer.subscribed){w::Subscribe value;value.instance=peer.hello.instance;value.connection_id=peer.connection;value.selection_epoch=peer.selection;value.qpc_frequency=frequency;value.pid=GetCurrentProcessId();value.process_creation_filetime=birth(GetCurrentProcess());w::Frame f;
     if(!w::encode(value,++sequence,f)){failure=ERROR_INVALID_DATA;break;}
     // A receiver continuation can run after the exporter has already responded.
     // Commit this pre-write lower bound only after successful Subscribe. The
     // exporter independently enforces observation after its actual receipt.
     const auto issued_floor=qpc_now();
     if(!write_frame(pipe.get(),f,actual,domain,started+static_cast<std::int64_t>(frequency))){failure=ERROR_WRITE_FAULT;break;}
     if(fixture_subscribe_release){HANDLE waits[]{cancel.get(),fixture_subscribe_release};if(WaitForMultipleObjects(2,waits,FALSE,1000)!=WAIT_OBJECT_0+1){failure=ERROR_OPERATION_ABORTED;break;}}
     peer.floor=issued_floor;peer.subscribed=true;TrafficEvent e;e.kind=TrafficEventKind::hello;e.process=actual;e.connection_id=peer.connection;e.selection_epoch=peer.selection;e.received_qpc=qpc_now();e.subscribe_floor_qpc=peer.floor;e.hello=peer.hello;if(!push(e)){failure=ERROR_BUFFER_OVERFLOW;break;}
    }
    if(peer.subscribed&&now-last_output>=static_cast<std::int64_t>(frequency/4)){
     if(sequence==UINT32_MAX){failure=ERROR_ARITHMETIC_OVERFLOW;break;}w::Heartbeat value{peer.hello.instance,peer.connection,peer.selection,qpc_now(),peer.capabilities,w::Reason::none};w::Frame f;
     if(!w::encode(value,++sequence,f)||!write_frame(pipe.get(),f,actual,domain)){failure=ERROR_WRITE_FAULT;break;}last_output=qpc_now();
    }
    DWORD count{};
    if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto completion=readiness_io::poll(pipe.get(),read.value);if(completion.pending)continue;read.pending=false;count=completion.bytes;if(!completion.success||!count){failure=completion.success?ERROR_BROKEN_PIPE:completion.error;break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}if(peer.hello_seen&&!peer.subscribed)continue;}
    if(!read.pending){read.prepare(pipe.get());if(ReadFile(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value)){if(!count||!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}continue;}if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else {failure=GetLastError();break;}}
    HANDLE waits[]{cancel.get(),read.event.get(),wake.get()};const auto result=WaitForMultipleObjects(3,waits,FALSE,20);if(result==WAIT_OBJECT_0+2)ResetEvent(wake.get());if(result==WAIT_FAILED){failure=GetLastError();break;}
   }
   read.cancel();DisconnectNamedPipe(pipe.get());lost(actual,peer.connection,peer.selection,failure?failure:ERROR_OPERATION_ABORTED,failure==ERROR_TIMEOUT?w::Reason::timeout:failure==ERROR_INVALID_DATA?w::Reason::protocol:w::Reason::disconnected);WaitForSingleObject(cancel.get(),250);
  }
 }
};
TrafficService::TrafficService():impl_(std::make_unique<Impl>()){}
TrafficService::~TrafficService(){request_stop();join();}
void TrafficService::start(std::uint64_t frequency){if(impl_->worker.joinable())return;{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->head=impl_->size=0;ResetEvent(impl_->ready.get());}impl_->frequency=frequency;ResetEvent(impl_->cancel.get());impl_->finished=false;impl_->worker=std::thread([p=impl_.get()]{try{p->run();}catch(...){p->lost({},0,0,ERROR_UNHANDLED_EXCEPTION,w::Reason::native_fault);}p->finished=true;});}
void TrafficService::associate(ProcessIdentity expected){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->expected!=expected){impl_->expected=expected;++impl_->epoch;++impl_->selection;SetEvent(impl_->wake.get());}}
void TrafficService::request_stop() noexcept {SetEvent(impl_->cancel.get());}
void TrafficService::join(){if(impl_->worker.joinable())impl_->worker.join();}
bool TrafficService::done() const noexcept {return impl_->finished;}
bool TrafficService::pop(TrafficEvent& e){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->size)return false;e=impl_->queue[impl_->head];impl_->head=(impl_->head+1)%impl_->queue.size();--impl_->size;if(!impl_->size)ResetEvent(impl_->ready.get());return true;}
std::uintptr_t TrafficService::ready_event() const noexcept {return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t TrafficService::dropped() const noexcept {return impl_->drops.load();}
void TrafficService::fault(std::uint32_t error) noexcept {ProcessIdentity process;std::uint64_t selection{};{std::lock_guard<std::mutex> lock(impl_->mutex);process=impl_->expected;selection=impl_->selection;++impl_->epoch;SetEvent(impl_->wake.get());}impl_->lost(process,0,selection,error,w::Reason::protocol);}
void TrafficService::fixture_name(std::wstring name){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->name=std::move(name);}
void TrafficService::fixture_subscribe_barrier(std::uintptr_t event){impl_->fixture_subscribe_release=reinterpret_cast<HANDLE>(event);}
}
