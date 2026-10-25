#include <psx/sync/worker_io.hpp>
#include "psx_plugin/traffic_service.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include "psx_plugin/motion_support.hpp"
#include "psx_plugin/observation_client.hpp"
#include <Windows.h>
#include <sddl.h>
#include <thread>
namespace psx::xp {
namespace {
struct Handle {HANDLE h{INVALID_HANDLE_VALUE};Handle()=default;explicit Handle(HANDLE p):h(p){}~Handle(){if(valid())CloseHandle(h);}bool valid()const noexcept{return h&&h!=INVALID_HANDLE_VALUE;}};
std::uint64_t birth(HANDLE h) noexcept {FILETIME c{},e{},k{},u{};if(!GetProcessTimes(h,&c,&e,&k,&u))return 0;return(static_cast<std::uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
std::wstring sid(HANDLE process){Handle token;token.h=nullptr;std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> data{};DWORD n{};if(!OpenProcessToken(process,TOKEN_QUERY,&token.h)||!GetTokenInformation(token.h,TokenUser,data.data(),static_cast<DWORD>(data.size()),&n))return {};LPWSTR raw{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&raw))return {};std::wstring value(raw);LocalFree(raw);return value;}
struct Operation {Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED op{};bool pending{};void prepare()noexcept{ResetEvent(event.h);op={};op.hEvent=event.h;}void cancel(HANDLE pipe)noexcept{if(pending){psx::sync::io_cancel(pipe,&op);DWORD n{};psx::sync::io_result(pipe,&op,&n,TRUE);pending=false;}}};
bool same_domain(const tv::Claim& a,const tv::Claim& b) noexcept {return a.plugin==b.plugin&&a.owner==b.owner&&a.connection==b.connection&&a.lease==b.lease&&a.source_producer==b.source_producer&&a.source_connection==b.source_connection&&a.source_selection==b.source_selection&&a.lifecycle==b.lifecycle&&a.source_pid==b.source_pid&&a.source_birth==b.source_birth&&a.frequency==b.frequency&&a.source_floor_qpc==b.source_floor_qpc&&a.presentation_mode==b.presentation_mode&&a.delay_ms==b.delay_ms&&a.model==b.model&&a.profile==b.profile;}
}
std::wstring traffic_endpoint(){const auto s=sid(GetCurrentProcess());const std::string prefix=tv::plugin_pipe_prefix;return s.empty()?std::wstring{}:std::wstring(prefix.begin(),prefix.end())+s+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(birth(GetCurrentProcess()));}
struct TrafficService::Impl {
 Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};std::thread worker;std::wstring endpoint;tv::Hello hello{};
 std::atomic<bool> connected{},fault{};std::atomic<std::uint64_t> transport{};std::atomic<std::uint32_t> capabilities{tv::supported};
 SpscFifo<TrafficCommand,16> controls;struct Reply{tv::Ack value{};std::uint64_t transport{};};SpscFifo<Reply,16> replies;
 TripleBuffer<TrafficWindow> windows;TripleBuffer<tv::Report> reports;tv::History history;tv::Claim claim{};
 TripleBuffer<tv::Capture> captures;tv::Capture capture_intent{};std::int64_t capture_floor{};SpscFifo<tv::Detail,128> details;std::atomic<std::uint64_t> dropped{};
 std::uint32_t sequence{};std::int64_t peer_time{},heartbeat{},report_time{};std::uint64_t last_request{};tv::Framer framer;bool have_claim{};tv::Report latest{};
 motion::InstanceId peer_owner{};std::uint64_t peer_connection{};bool have_peer{};
 ~Impl(){SetEvent(stop.h);if(worker.joinable())worker.join();}
 static bool frame(void* ctx,const tv::Header& h,const std::uint8_t* data,std::size_t size)noexcept{
  auto& s=*static_cast<Impl*>(ctx);const auto now=observation_qpc();
  if(h.kind==tv::Kind::capture){tv::Capture c;if(!tv::decode(data,size,c)||c.plugin!=s.hello.instance||c.sent_qpc>now||now-c.sent_qpc>=static_cast<std::int64_t>(s.hello.qpc_frequency*2)|| (s.have_claim&&c.connection!=s.claim.connection))return false;if(c.generation<s.capture_intent.generation)return true;if(c.generation!=s.capture_intent.generation||c.enabled!=s.capture_intent.enabled)s.capture_floor=now;s.capture_intent=c;s.captures.writing()=c;s.captures.publish();s.peer_time=now;return true;}
  if(h.kind==tv::Kind::claim){tv::Claim c;if(!tv::decode(data,size,c)||!tv::valid(c)||c.plugin!=s.hello.instance||c.frequency!=s.hello.qpc_frequency||c.request<=s.last_request||c.source_floor_qpc>now)return false;
   if(s.have_peer&&(c.owner!=s.peer_owner||c.connection!=s.peer_connection))return false;s.peer_owner=c.owner;s.peer_connection=c.connection;s.have_peer=true;
   if(s.have_claim&&c.action!=tv::Action::acquire&&!same_domain(c,s.claim))return false;
   if(c.action==tv::Action::acquire){s.history.clear();s.latest={};}s.claim=c;s.have_claim=true;s.last_request=c.request;s.peer_time=now;
   return s.controls.push({c,s.transport.load(std::memory_order_relaxed),now});
  }
  if(h.kind==tv::Kind::heartbeat){tv::Heartbeat v;if(!tv::decode(data,size,v)||(s.have_peer&&(v.sender!=s.peer_owner||v.connection!=s.peer_connection))||v.generated_qpc>now||now-v.generated_qpc>=static_cast<std::int64_t>(s.hello.qpc_frequency*5))return false;s.peer_owner=v.sender;s.peer_connection=v.connection;s.have_peer=true;s.peer_time=now;return true;}
  if(h.kind==tv::Kind::bundle){tv::Bundle b;if(!tv::decode(data,size,b)||!tv::valid(b)||!s.have_claim)return false;
   const auto& c=s.claim;const auto& src=b.source;const auto& n=src.snapshot.header;
   if(b.plugin!=c.plugin||b.owner!=c.owner||b.connection!=c.connection||b.lease!=c.lease)return false;
   if(c.action==tv::Action::release)return true;
   if(src.producer!=c.source_producer||src.connection_id!=c.source_connection||src.selection_epoch!=c.source_selection||n.lifecycle!=c.lifecycle||n.process!=c.source_pid||n.process_created!=c.source_birth||n.frequency!=c.frequency)return false;
   const auto age_now=psx::sync::freshness_now(now);
   if(n.observed<=static_cast<std::uint64_t>(c.source_floor_qpc)||n.finished>static_cast<std::uint64_t>(age_now)||age_now-static_cast<std::int64_t>(n.observed)>=static_cast<std::int64_t>(c.frequency))return true;
   if(s.history.accept(src,now,age_now-now)){s.windows.writing()={s.history.window(),s.transport.load(std::memory_order_relaxed),c.lease};s.windows.publish();}s.peer_time=now;return true;
  }return false;
 }
 bool next(tv::Frame& f,std::int64_t now)noexcept{
  Reply r;for(unsigned i=0;i<16&&replies.pop(r);++i)if(r.transport==transport.load(std::memory_order_acquire))return tv::encode(r.value,++sequence,f);
  if(have_peer&&(!heartbeat||now-heartbeat>=static_cast<std::int64_t>(hello.qpc_frequency/4))){heartbeat=now;return tv::encode(tv::Heartbeat{hello.instance,peer_connection,now,capabilities.load(std::memory_order_acquire),tv::Reason::none},++sequence,f);}
  if(reports.consume())latest=reports.reading();
  if(have_claim&&latest.plugin==hello.instance&&latest.owner==claim.owner&&latest.connection==claim.connection&&latest.lease==claim.lease&&latest.request==claim.request&&latest.generated_qpc>report_time){report_time=latest.generated_qpc;return tv::encode(latest,++sequence,f);}
  tv::Detail d;for(unsigned i=0;i<128&&details.pop(d);++i)if(capture_intent.enabled&&capture_intent.generation==d.capture_generation&&d.connection==capture_intent.connection&&d.generated_qpc>=capture_floor&&now-capture_intent.sent_qpc<static_cast<std::int64_t>(hello.qpc_frequency*2)){d.dropped=dropped.load(std::memory_order_relaxed);if(d.dropped)d.flags|=tv::detail_incomplete;return tv::encode(d,++sequence,f);}return false;
 }
 void serve(HANDLE pipe,HANDLE peer,std::uint64_t peer_birth)noexcept{
  Operation read,write;std::array<std::uint8_t,4096> input{};tv::Frame output;
  if(!read.event.valid()||!write.event.valid())return;
  history.clear();claim={};capture_intent={};have_claim=have_peer=false;peer_owner={};peer_connection=0;last_request=0;sequence=0;heartbeat=report_time=0;framer.reset();fault.store(false);transport.fetch_add(1);connected.store(true,std::memory_order_release);
  hello.capabilities=capabilities.load();bool ready=tv::encode(hello,++sequence,output);std::size_t offset{};const auto started=observation_qpc();peer_time=started;auto progress=started;
  while(WaitForSingleObject(stop.h,0)==WAIT_TIMEOUT&&!fault.load(std::memory_order_acquire)){
   if(!psx::sync::worker_poll(pipe))break;
   const auto now=observation_qpc();if((peer&&(WaitForSingleObject(peer,0)!=WAIT_TIMEOUT||birth(peer)!=peer_birth))||now-peer_time>=static_cast<std::int64_t>(hello.qpc_frequency*5)||(!have_peer&&now-started>=static_cast<std::int64_t>(hello.qpc_frequency))||(write.pending&&now-progress>=static_cast<std::int64_t>(hello.qpc_frequency))||sequence>=UINT32_MAX-8)break;
   bool failed=false;
   for(unsigned batch=0;batch<8;++batch){DWORD count{};if(read.pending){if(WaitForSingleObject(read.event.h,0)!=WAIT_OBJECT_0)break;if(!psx::sync::io_result(pipe,&read.op,&count,FALSE)){failed=true;break;}read.pending=false;}else{read.prepare();if(!psx::sync::io_read(pipe,input.data(),static_cast<DWORD>(input.size()),&count,&read.op)){if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}read.pending=true;break;}}if(!count||!framer.feed(input.data(),count,frame,this)){failed=true;break;}}
   if(failed)break;
   if(write.pending&&WaitForSingleObject(write.event.h,0)==WAIT_OBJECT_0){DWORD n{};if(!psx::sync::io_result(pipe,&write.op,&n,FALSE)||!n)break;write.pending=false;offset+=n;progress=now;if(offset==output.size){ready=false;offset=0;}}
   for(unsigned batch=0;batch<8&&!write.pending;++batch){if(!ready){ready=next(output,now);offset=0;if(!ready)break;}write.prepare();DWORD n{};progress=now;if(!psx::sync::io_write(pipe,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset),&n,&write.op)){if(GetLastError()!=ERROR_IO_PENDING){failed=true;break;}write.pending=true;break;}if(!n){failed=true;break;}offset+=n;if(offset==output.size){ready=false;offset=0;}}
   if(failed)break;if(!read.pending)ResetEvent(read.event.h);if(!write.pending)ResetEvent(write.event.h);HANDLE waits[]{stop.h,read.event.h,write.event.h};WaitForMultipleObjects(3,waits,FALSE,5);
  }connected.store(false,std::memory_order_release);read.cancel(pipe);write.cancel(pipe);
 }
 void run()noexcept{while(WaitForSingleObject(stop.h,0)==WAIT_TIMEOUT){if(psx::sync::enabled()){psx::sync::WorkerIo io(psx::sync::Family::traffic);serve(nullptr,nullptr,io.status().peer.process_birth);}else run_local();WaitForSingleObject(stop.h,20);}}
 void run_local()noexcept{if(endpoint.empty())return;try{  const auto own=sid(GetCurrentProcess());if(own.empty())return;const auto acl=L"D:P(A;;GA;;;SY)(A;;GA;;;"+own+L")";PSECURITY_DESCRIPTOR raw{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&raw,nullptr))return;
  struct Descriptor{PSECURITY_DESCRIPTOR p;~Descriptor(){LocalFree(p);}} descriptor{raw};SECURITY_ATTRIBUTES sa{sizeof(sa),raw,FALSE};
  Handle pipe(CreateNamedPipeW(endpoint.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&sa));if(!pipe.valid())return;
  Operation connect;if(!connect.event.valid())return;
  while(psx::sync::local_io_selected()&&WaitForSingleObject(stop.h,0)==WAIT_TIMEOUT){connect.prepare();bool attached=ConnectNamedPipe(pipe.h,&connect.op)!=FALSE;if(!attached){const auto e=GetLastError();if(e==ERROR_PIPE_CONNECTED)attached=true;else if(e==ERROR_IO_PENDING){connect.pending=true;HANDLE waits[]{stop.h,connect.event.h};if(psx::sync::wait_local_connection(stop.h,waits[1])==WAIT_OBJECT_0+1){DWORD n{};attached=psx::sync::io_result(pipe.h,&connect.op,&n,FALSE)!=FALSE;connect.pending=false;}else{connect.cancel(pipe.h);break;}}else break;}
   ULONG pid{};if(attached&&GetNamedPipeClientProcessId(pipe.h,&pid)&&pid){Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));const auto born=peer.valid()?birth(peer.h):0;if(born&&sid(peer.h)==own)serve(pipe.h,peer.h,born);}DisconnectNamedPipe(pipe.h);
  }
 }catch(...){}connected.store(false,std::memory_order_release);}
};
TrafficService::TrafficService()=default;TrafficService::~TrafficService(){stop();}
bool TrafficService::start(motion::InstanceId id,const std::wstring& endpoint){stop();if(!motion::valid(id))return false;auto p=std::make_unique<Impl>();p->endpoint=endpoint.empty()?traffic_endpoint():endpoint;if((p->endpoint.empty()&&!psx::sync::runtime())||!p->stop.valid())return false;p->hello={id,GetCurrentProcessId(),tv::supported,birth(GetCurrentProcess()),observation_qpc_frequency()};auto* raw=p.get();raw->worker=std::thread([raw]{raw->run();});impl_=std::move(p);return true;}
void TrafficService::stop()noexcept{impl_.reset();}
bool TrafficService::connected()const noexcept{return impl_&&impl_->connected.load(std::memory_order_acquire)&&!impl_->fault.load(std::memory_order_acquire);}
std::uint64_t TrafficService::connection()const noexcept{return connected()?impl_->transport.load(std::memory_order_acquire):0;}
bool TrafficService::command(TrafficCommand& c)noexcept{if(!impl_)return false;for(unsigned i=0;i<16&&impl_->controls.pop(c);++i)if(c.transport==connection())return true;return false;}
bool TrafficService::window(TrafficWindow& w)noexcept{if(!impl_||!impl_->windows.consume())return false;w=impl_->windows.reading();return w.transport==connection();}
void TrafficService::ack(const tv::Ack& a,std::uint64_t transport)noexcept{if(impl_&&!impl_->replies.push({a,transport}))impl_->fault.store(true,std::memory_order_release);}
void TrafficService::publish(const tv::Report& r,std::uint32_t cap)noexcept{if(!impl_)return;impl_->capabilities.store(cap,std::memory_order_release);impl_->reports.writing()=r;impl_->reports.publish();}
bool TrafficService::capture(tv::Capture& c)noexcept{if(!impl_||!impl_->captures.consume())return false;c=impl_->captures.reading();return connected();}
void TrafficService::detail(tv::Detail d)noexcept{if(impl_&&!impl_->details.push(d))impl_->dropped.fetch_add(1,std::memory_order_relaxed);}
}
