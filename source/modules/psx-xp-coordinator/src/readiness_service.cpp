#include <psx/sync/worker_io.hpp>
#include <optional>
#include "coordinator/readiness.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/status.hpp"
#include "readiness_completion.hpp"
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
namespace coordinator {
namespace s=status_wire;
namespace {
std::uint64_t birth(HANDLE h){FILETIME a{},b{},c{},d{};return GetProcessTimes(h,&a,&b,&c,&d)?std::uint64_t(a.dwLowDateTime)|(std::uint64_t(a.dwHighDateTime)<<32):0;}
std::wstring sid(HANDLE process){HANDLE raw{};if(!OpenProcessToken(process,TOKEN_QUERY,&raw))return {};WinHandle token(raw);DWORD n{};GetTokenInformation(token.get(),TokenUser,nullptr,0,&n);std::vector<std::uint8_t> data(n);if(!n||!GetTokenInformation(token.get(),TokenUser,data.data(),n,&n))return {};LPWSTR str{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&str))return {};std::wstring result(str);LocalFree(str);return result;}
struct Operation {
 WinHandle event{make_stop_event()};OVERLAPPED value{};bool pending{};HANDLE target{};
 void prepare(HANDLE h){target=h;ResetEvent(event.get());value={};value.hEvent=event.get();}
 // Cancellation requests do not retire an operation. The worker retains its
 // handle, OVERLAPPED and buffer until Windows reports actual completion.
 void cancel(){if(pending){psx::sync::io_cancel(target,&value);DWORD n{};psx::sync::io_result(target,&value,&n,TRUE);pending=false;}}
 ~Operation(){cancel();}
};
void header(s::Frame& f,const s::Header& h){f.size=s::header_size+h.payload_size;f.bytes[0]='P';f.bytes[1]='X';f.bytes[2]='S';f.bytes[3]='1';f.bytes[4]=1;f.bytes[5]=0;for(unsigned n=0;n<2;++n)f.bytes[6+n]=static_cast<std::uint8_t>(static_cast<unsigned>(h.kind)>>(n*8));for(unsigned n=0;n<4;++n){f.bytes[8+n]=static_cast<std::uint8_t>(h.payload_size>>(n*8));f.bytes[12+n]=static_cast<std::uint8_t>(h.sequence>>(n*8));}}
}
std::wstring readiness_plugin_pipe_name(){const auto user=sid(GetCurrentProcess());if(user.empty())throw_win32_error("Readiness SID");return L"\\\\.\\pipe\\psx-ios.status.plugin.v1."+user;}
std::wstring readiness_exporter_pipe_name(ProcessIdentity p){const auto user=sid(GetCurrentProcess());if(user.empty())throw_win32_error("Readiness SID");return L"\\\\.\\pipe\\psx-ios.status.exporter.v1."+user+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);}
struct ReadinessService::Impl {
 struct Configuration {ProcessIdentity expected{};std::wstring name;std::uint64_t fault_epoch{};WinHandle wake{make_stop_event()};};
 mutable std::mutex mutex;std::array<Configuration,2> config;
 WinHandle cancel{make_stop_event()},ready{make_stop_event()};std::array<std::thread,2> workers;
 std::array<std::atomic<bool>,2> finished{{true,true}};
 std::array<ReadinessEvent,256> queue{};std::size_t head{},size{};
 std::atomic<std::uint64_t> dropped{},connections{};std::uint64_t frequency{};
 bool push(const ReadinessEvent& e){std::lock_guard<std::mutex> lock(mutex);if(size==queue.size()){
   dropped+=size+1;head=size=0;for(std::size_t i=0;i<2;++i){ReadinessEvent loss;loss.role=i?s::Role::exporter:s::Role::plugin;loss.received_qpc=e.received_qpc;loss.error=ERROR_BUFFER_OVERFLOW;queue[size++]=loss;++config[i].fault_epoch;SetEvent(config[i].wake.get());}SetEvent(ready.get());return false;}
  queue[(head+size)%queue.size()]=e;++size;SetEvent(ready.get());return true;}
 bool active(std::size_t i,ProcessIdentity actual,std::uint64_t epoch){if(WaitForSingleObject(cancel.get(),0)==WAIT_OBJECT_0)return false;std::lock_guard<std::mutex> lock(mutex);return epoch==config[i].fault_epoch&&(!config[i].expected.pid||config[i].expected==actual);}
 void lost(std::size_t i,ProcessIdentity p,std::uint64_t c,DWORD error){ReadinessEvent e;e.role=i?s::Role::exporter:s::Role::plugin;e.process=p;e.connection_id=c;e.error=error;e.received_qpc=qpc_now();push(e);}
 struct Peer {
  Impl* self{};std::size_t index{};ProcessIdentity process{};s::Hello hello{};bool hello_seen{},subscribed{};std::uint64_t connection{};std::int64_t floor{},last_input{};
  static bool receive(void* context,const s::Header& h,const std::uint8_t* data,std::size_t n){auto& p=*static_cast<Peer*>(context);auto& self=*p.self;const auto now=qpc_now();
   if(!p.hello_seen){if(h.kind!=s::Kind::hello||!s::decode(data,n,p.hello)||p.hello.role!=(p.index?s::Role::exporter:s::Role::plugin)||p.hello.pid!=p.process.pid||p.hello.process_creation_filetime!=p.process.creation_filetime||p.hello.qpc_frequency!=self.frequency)return false;p.hello_seen=true;p.last_input=now;return true;}
   if(!p.subscribed)return false;
   if(h.kind==s::Kind::heartbeat){s::Heartbeat v;if(!s::decode(data,n,v)||v.producer!=p.hello.instance||v.connection_id!=p.connection||v.generated_qpc<p.floor||v.generated_qpc>now||(v.capabilities&~(p.index?s::fatal_state:s::xplane_readiness)))return false;}
   else if(h.kind==s::Kind::snapshot){s::Snapshot v;if(!s::decode(data,n,v)||v.role!=p.hello.role||v.producer!=p.hello.instance||v.connection_id!=p.connection||v.observed_qpc>now||v.phase_start_qpc>v.observed_qpc||v.call_returned_qpc>v.observed_qpc||v.notification_qpc>v.observed_qpc)return false;}
   else return false;
   ReadinessEvent e;e.kind=ReadinessEventKind::frame;e.role=p.hello.role;e.process=p.process;e.connection_id=p.connection;e.subscribe_floor_qpc=p.floor;e.received_qpc=now;header(e.frame,h);std::copy(data,data+n,e.frame.bytes.begin()+s::header_size);p.last_input=now;return self.push(e);
  }
 };
 bool write_frame(HANDLE pipe,s::Frame& frame,std::size_t i,ProcessIdentity p,std::uint64_t epoch){Operation op;std::size_t offset{};const auto started=qpc_now();while(offset<frame.size&&active(i,p,epoch)){
   DWORD n{};op.prepare(pipe);if(psx::sync::io_write(pipe,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset),&n,&op.value)){if(!n)return false;offset+=n;continue;}
   if(GetLastError()!=ERROR_IO_PENDING)return false;op.pending=true;
   while(active(i,p,epoch)&&qpc_now()-started<static_cast<std::int64_t>(frequency*2)){if(!psx::sync::worker_poll(pipe))return false;HANDLE waits[]={cancel.get(),op.event.get(),config[i].wake.get()};const auto w=WaitForMultipleObjects(3,waits,FALSE,2);if(w==WAIT_OBJECT_0+1){const auto result=readiness_io::poll(pipe,op.value);if(result.pending)continue;op.pending=false;if(!result.success||!result.bytes)return false;offset+=result.bytes;break;}if(w==WAIT_OBJECT_0+2)ResetEvent(config[i].wake.get());if(w==WAIT_FAILED)return false;}
   if(op.pending)return false;
  }return offset==frame.size;
 }
 void run(std::size_t i){const bool remote=i==0&&psx::sync::enabled();const auto user=remote?std::wstring{}:sid(GetCurrentProcess());if(!remote&&user.empty())throw_win32_error("Readiness user token");const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+user+L")";
  while(WaitForSingleObject(cancel.get(),0)!=WAIT_OBJECT_0){ProcessIdentity expected;std::wstring name;std::uint64_t epoch{};{std::lock_guard<std::mutex> lock(mutex);expected=config[i].expected;name=config[i].name;epoch=config[i].fault_epoch;ResetEvent(config[i].wake.get());}
   if((i||remote)&&!expected.pid){HANDLE waits[]={cancel.get(),config[i].wake.get()};WaitForMultipleObjects(2,waits,FALSE,100);continue;}
   if(!remote&&name.empty())name=i?readiness_exporter_pipe_name(expected):readiness_plugin_pipe_name();
   PSECURITY_DESCRIPTOR raw{};if(!remote&&!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr)){lost(i,expected,0,GetLastError());WaitForSingleObject(cancel.get(),100);continue;}
   SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),raw,FALSE};WinHandle pipe(remote?nullptr:CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,8192,8192,0,&security));LocalFree(raw);
   if(!remote&&!pipe.valid()){lost(i,expected,0,GetLastError());WaitForSingleObject(cancel.get(),100);continue;}
   std::optional<psx::sync::WorkerIo> network;if(remote){const auto state=psx::sync::runtime()->status();if(!state.connected||state.peer.pid!=expected.pid||state.peer.process_birth!=expected.creation_filetime){WaitForSingleObject(cancel.get(),20);continue;}network.emplace(psx::sync::Family::status);}
   Operation connection;connection.prepare(pipe.get());bool attached=remote||ConnectNamedPipe(pipe.get(),&connection.value)!=FALSE;
   if(!attached){const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)attached=true;else if(error==ERROR_IO_PENDING){connection.pending=true;while(active(i,expected,epoch)){HANDLE waits[]={cancel.get(),connection.event.get(),config[i].wake.get()};const auto w=WaitForMultipleObjects(3,waits,FALSE,20);if(w==WAIT_OBJECT_0+1){const auto result=readiness_io::poll(pipe.get(),connection.value);if(result.pending)continue;attached=result.success;connection.pending=false;break;}if(w==WAIT_OBJECT_0+2)ResetEvent(config[i].wake.get());if(w==WAIT_FAILED)break;}}}
   connection.cancel();if(!attached)continue;
   ULONG pid=remote?expected.pid:0;WinHandle process;if(!remote&&GetNamedPipeClientProcessId(pipe.get(),&pid))process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));ProcessIdentity actual=remote?expected:ProcessIdentity{pid,process.valid()?birth(process.get()):0};
   if((!remote&&(!process.valid()||sid(process.get())!=user))||!actual.creation_filetime||!active(i,actual,epoch)){lost(i,actual,0,ERROR_ACCESS_DENIED);if(!remote)DisconnectNamedPipe(pipe.get());WaitForSingleObject(cancel.get(),100);continue;}
   Peer peer;peer.self=this;peer.index=i;peer.process=actual;peer.connection=++connections;peer.last_input=qpc_now();s::Framer framer;Operation read;std::array<std::uint8_t,4096> input{};std::uint32_t sequence{};std::int64_t last_output{};DWORD failure{};
   while(active(i,actual,epoch)&&(remote||WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)){if(!psx::sync::worker_poll(pipe.get()))break;const auto now=qpc_now();if(now-peer.last_input>=static_cast<std::int64_t>(frequency*(peer.subscribed?5:2))){failure=ERROR_TIMEOUT;break;}
    if(peer.hello_seen&&!peer.subscribed){s::Subscribe subscription;subscription.instance=peer.hello.instance;subscription.connection_id=peer.connection;subscription.qpc_frequency=frequency;subscription.pid=GetCurrentProcessId();s::Frame f;peer.floor=qpc_now();if(!s::encode(subscription,++sequence,f)||!write_frame(pipe.get(),f,i,actual,epoch)){failure=ERROR_WRITE_FAULT;break;}
     // The peer can already be producing replies when asynchronous submission
     // completes. Its causal floor is submission, never write completion.
     peer.subscribed=true;ReadinessEvent e;e.kind=ReadinessEventKind::hello;e.role=peer.hello.role;e.process=actual;e.connection_id=peer.connection;e.received_qpc=qpc_now();e.subscribe_floor_qpc=peer.floor;e.hello=peer.hello;if(!push(e)){failure=ERROR_BUFFER_OVERFLOW;break;}
    }
    if(peer.subscribed&&now-last_output>=static_cast<std::int64_t>(frequency/4)){if(sequence==std::numeric_limits<std::uint32_t>::max()){failure=ERROR_ARITHMETIC_OVERFLOW;break;}s::Heartbeat v;v.producer=peer.hello.instance;v.connection_id=peer.connection;v.generated_qpc=qpc_now();v.capabilities=peer.hello.capabilities;s::Frame f;if(!s::encode(v,++sequence,f)||!write_frame(pipe.get(),f,i,actual,epoch)){failure=ERROR_WRITE_FAULT;break;}last_output=qpc_now();}
    DWORD count{};
    if(read.pending&&WaitForSingleObject(read.event.get(),0)==WAIT_OBJECT_0){const auto result=readiness_io::poll(pipe.get(),read.value);if(result.pending)continue;read.pending=false;count=result.bytes;if(!result.success||!count){failure=result.success?ERROR_BROKEN_PIPE:result.error;break;}if(!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}if(peer.hello_seen&&!peer.subscribed)continue;}
    if(!read.pending){read.prepare(pipe.get());if(psx::sync::io_read(pipe.get(),input.data(),static_cast<DWORD>(input.size()),&count,&read.value)){if(!count||!framer.feed(input.data(),count,Peer::receive,&peer)){failure=ERROR_INVALID_DATA;break;}continue;}if(GetLastError()==ERROR_IO_PENDING)read.pending=true;else{failure=GetLastError();break;}}
    HANDLE waits[]={cancel.get(),read.event.get(),config[i].wake.get()};const auto w=WaitForMultipleObjects(3,waits,FALSE,20);if(w==WAIT_OBJECT_0+2)ResetEvent(config[i].wake.get());if(w==WAIT_FAILED){failure=GetLastError();break;}
   }
   read.cancel();DisconnectNamedPipe(pipe.get());lost(i,actual,peer.connection,failure?failure:ERROR_OPERATION_ABORTED);WaitForSingleObject(cancel.get(),100);
  }
 }
};
ReadinessService::ReadinessService():impl_(std::make_unique<Impl>()){}
ReadinessService::~ReadinessService(){request_stop();join();}
void ReadinessService::start(std::uint64_t f){if(impl_->workers[0].joinable())return;{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->head=impl_->size=0;ResetEvent(impl_->ready.get());}impl_->frequency=f;ResetEvent(impl_->cancel.get());for(std::size_t i=0;i<2;++i){impl_->finished[i]=false;impl_->workers[i]=std::thread([p=impl_.get(),i]{try{p->run(i);}catch(...){p->lost(i,{},0,ERROR_UNHANDLED_EXCEPTION);}p->finished[i]=true;});}}
void ReadinessService::associate(ProcessIdentity p){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->config[1].expected!=p){impl_->config[1].expected=p;++impl_->config[1].fault_epoch;SetEvent(impl_->config[1].wake.get());}}
void ReadinessService::plugin(ProcessIdentity p){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->config[0].expected=p;SetEvent(impl_->config[0].wake.get());}
void ReadinessService::request_stop() noexcept {SetEvent(impl_->cancel.get());}
void ReadinessService::join(){for(auto& worker:impl_->workers)if(worker.joinable())worker.join();}
bool ReadinessService::done() const noexcept {return impl_->finished[0]&&impl_->finished[1];}
bool ReadinessService::pop(ReadinessEvent& e){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->size)return false;e=impl_->queue[impl_->head];impl_->head=(impl_->head+1)%impl_->queue.size();if(!--impl_->size)ResetEvent(impl_->ready.get());return true;}
std::uintptr_t ReadinessService::ready_event() const noexcept {return reinterpret_cast<std::uintptr_t>(impl_->ready.get());}
std::uint64_t ReadinessService::dropped() const noexcept {return impl_->dropped.load();}
void ReadinessService::fault(s::Role role,std::uint32_t error) noexcept {const auto i=role==s::Role::exporter?1:0;ProcessIdentity p;{std::lock_guard<std::mutex> lock(impl_->mutex);p=impl_->config[i].expected;++impl_->config[i].fault_epoch;SetEvent(impl_->config[i].wake.get());}impl_->lost(i,p,0,error);}
void ReadinessService::fixture_names(std::wstring a,std::wstring b){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->config[0].name=std::move(a);impl_->config[1].name=std::move(b);}
}
