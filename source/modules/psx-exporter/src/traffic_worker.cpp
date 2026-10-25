#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psx_exporter/traffic_runtime.hpp>
#include <atomic>
#include <array>
#include <algorithm>
#include <cstring>
namespace psx_exporter {
namespace {
namespace w=psx::traffic_source;
std::int64_t ticks() noexcept {LARGE_INTEGER n{};QueryPerformanceCounter(&n);return n.QuadPart;}
struct Handle {HANDLE h{INVALID_HANDLE_VALUE};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}bool valid() const noexcept{return h&&h!=INVALID_HANDLE_VALUE;}};
struct Operation {
 Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};OVERLAPPED value{};HANDLE pipe{};DWORD done{};bool pending{};
 ~Operation(){cancel();}
 bool begin(HANDLE h,bool writing,void* data,DWORD size) noexcept {
  if(!event.valid()||pending)return false;pipe=h;ResetEvent(event.h);value={};value.hEvent=event.h;done=0;
  const auto ok=writing?WriteFile(h,data,size,&done,&value):ReadFile(h,data,size,&done,&value);pending=!ok&&GetLastError()==ERROR_IO_PENDING;return ok||pending;
 }
 bool complete() noexcept {
  if(!pending)return true;DWORD size{};
  if(!GetOverlappedResult(pipe,&value,&size,FALSE))return false;done=size;pending=false;return true;
 }
 void cancel() noexcept {if(pending){CancelIoEx(pipe,&value);DWORD n{};GetOverlappedResult(pipe,&value,&n,TRUE);pending=false;}}
};
struct Runtime {
 w::Hello hello{};std::wstring endpoint;std::atomic<bool> started{};
 psx::traffic::Snapshot latest{};bool have{};std::uint64_t drained{},sent{},skips{},last_sequence{},work{},maximum{};
} runtime;
void drain() noexcept {
 if(!own_traffic_reader())return;
 psx::traffic::Snapshot s{};
 if(take_traffic(s)){
  if(runtime.last_sequence&&s.header.sequence>runtime.last_sequence+1)runtime.skips+=s.header.sequence-runtime.last_sequence-1;
  runtime.last_sequence=s.header.sequence;runtime.latest=s;runtime.have=true;++runtime.drained;publish_traffic_diagnostic(s);
 }
 if(!traffic_capabilities())runtime.have=false;
}
void measurement(std::int64_t begin) noexcept {const auto n=static_cast<std::uint64_t>(ticks()-begin);runtime.work+=n;runtime.maximum=std::max(runtime.maximum,n);}
bool same_user(HANDLE other) noexcept {
 Handle a{},b{};a.h=nullptr;b.h=nullptr;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&a.h)||!OpenProcessToken(other,TOKEN_QUERY,&b.h))return false;
 alignas(TOKEN_USER) std::array<std::uint8_t,4096> x{},y{};DWORD xn{},yn{};
 return GetTokenInformation(a.h,TokenUser,x.data(),static_cast<DWORD>(x.size()),&xn)&&GetTokenInformation(b.h,TokenUser,y.data(),static_cast<DWORD>(y.size()),&yn)&&EqualSid(reinterpret_cast<TOKEN_USER*>(x.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(y.data())->User.Sid);
}
std::uint64_t birth(HANDLE p) noexcept {FILETIME c{},e{},k{},u{};return GetProcessTimes(p,&c,&e,&k,&u)?(std::uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime:0;}
struct Peer {
 std::uint32_t pid{};std::uint64_t created{},connection{},selection{};std::int64_t floor{},received{};bool subscribed{};
 static bool receive(void* context,const w::Header& header,const std::uint8_t* data,std::size_t size) noexcept {
  auto& p=*static_cast<Peer*>(context);
  if(!p.subscribed){w::Subscribe s;if(header.kind!=w::Kind::subscribe||!w::decode(data,size,s)||s.instance!=runtime.hello.instance||s.pid!=p.pid||s.process_creation_filetime!=p.created||s.qpc_frequency!=runtime.hello.qpc_frequency)return false;p.connection=s.connection_id;p.selection=s.selection_epoch;p.subscribed=true;p.floor=ticks();}
  else {w::Heartbeat h;if(header.kind!=w::Kind::heartbeat||!w::decode(data,size,h)||h.producer!=runtime.hello.instance||h.connection_id!=p.connection||h.selection_epoch!=p.selection)return false;}
  p.received=ticks();return true;
 }
};
struct Ordered {
 std::array<w::Frame,16> frames{};std::size_t head{},size{};
 bool push(const w::Frame& f) noexcept {if(size==frames.size())return false;frames[(head+size)%frames.size()]=f;++size;return true;}
 bool pop(w::Frame& f) noexcept {if(!size)return false;f=frames[head];head=(head+1)%frames.size();--size;return true;}
};
bool service(HANDLE pipe) noexcept {
 Peer p;ULONG pid{};if(!GetNamedPipeServerProcessId(pipe,&pid)||!pid)return false;p.pid=pid;
 Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)};
 if(!process.valid()||WaitForSingleObject(process.h,0)!=WAIT_TIMEOUT||!(p.created=birth(process.h))||!same_user(process.h))return false;
 Operation read,write;std::array<std::uint8_t,w::max_frame> bytes{};w::Framer framer;Ordered ordered;
 auto hello=runtime.hello;hello.capabilities=traffic_capabilities();w::Frame output{};std::uint32_t sequence=1;
 if(!w::encode(hello,sequence++,output)||!ordered.push(output)||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
 std::size_t offset{};bool writing=false;const auto started=ticks();p.received=started;
 const auto frequency=static_cast<std::int64_t>(hello.qpc_frequency);std::int64_t progress=started,heartbeat=started,status=0,drain_at=0,sent_at=0;
 std::uint32_t previous_cap=~0u;w::Reason previous_reason=w::Reason::incompatible;std::uint64_t sent_source{};
 for(;;){
  const auto begin=ticks();auto now=begin;
  if((!p.subscribed&&now-started>=frequency)||(p.subscribed&&now-p.received>=frequency*5)||(writing&&now-progress>=frequency)||WaitForSingleObject(process.h,0)!=WAIT_TIMEOUT)break;
  if(now>=drain_at){drain();drain_at=now+(frequency+99)/100;}
  if(!read.pending||WaitForSingleObject(read.event.h,0)==WAIT_OBJECT_0){if(!read.complete()||!read.done||!framer.feed(bytes.data(),read.done,Peer::receive,&p)||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))break;}
  // A Subscribe can complete during the read above. Outgoing protocol facts
  // must be stamped after that processing, rather than at the loop entry.
  now=ticks();
  if(p.subscribed){
   const auto cap=traffic_capabilities();const auto why=traffic_reason();
   if(cap!=previous_cap||why!=previous_reason||now-status>=frequency){
    w::Status s{hello.instance,p.connection,p.selection,now,cap,why,runtime.drained,runtime.sent,runtime.skips,runtime.work,runtime.maximum};w::Frame f;
    if(!w::encode(s,sequence++,f)||!ordered.push(f))break;previous_cap=cap;previous_reason=why;status=now;
   }
   if(now-heartbeat>=frequency/4){w::Heartbeat h{hello.instance,p.connection,p.selection,now,cap,why};w::Frame f;if(!w::encode(h,sequence++,f)||!ordered.push(f))break;heartbeat=now;}
  }
  if(!writing){
   bool have=ordered.pop(output);
   if(!have&&p.subscribed&&runtime.have&&traffic_capabilities()&&now>=sent_at){const auto& h=runtime.latest.header;
    if(h.sequence>sent_source&&h.observed>static_cast<std::uint64_t>(p.floor)&&h.finished<=static_cast<std::uint64_t>(now+1)&&now-static_cast<std::int64_t>(h.observed)<frequency){
     w::Sample s{hello.instance,p.connection,p.selection,runtime.latest};have=w::encode(s,sequence++,output);if(!have)break;sent_source=h.sequence;sent_at=now+(frequency+99)/100;
    }
   }
   if(have){offset=0;writing=true;progress=now;if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size)))break;}
  }
  if(writing&&(!write.pending||WaitForSingleObject(write.event.h,0)==WAIT_OBJECT_0)){
   if(!write.complete()||!write.done)break;offset+=write.done;progress=ticks();
   if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset)))break;}
   else {w::Header h;if(w::read_header(output.bytes.data(),output.size,h)&&h.kind==w::Kind::sample)++runtime.sent;writing=false;}
  }
  measurement(begin);
  HANDLE events[]{read.event.h,write.event.h};const DWORD count=writing&&write.pending?2u:1u;
  // Immediate partial progress is serviced again without a timer delay. Idle
  // worker waits never occur on a native callback or another service thread.
  if(read.pending&&(!writing||write.pending))WaitForMultipleObjects(count,events,FALSE,10);
 }
 // Both operations retain buffers/OVERLAPPED until actual completion.
 read.cancel();write.cancel();return false;
}
DWORD WINAPI worker(void*) noexcept {
 std::int64_t reconnect{};const auto frequency=static_cast<std::int64_t>(runtime.hello.qpc_frequency);
 for(;;){const auto begin=ticks();drain();measurement(begin);
  const auto now=ticks();
  if(now>=reconnect){Handle pipe{CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr)};if(pipe.valid())service(pipe.h);reconnect=ticks()+frequency/4;}
  Sleep(10);
 }
}
}
void start_traffic(const w::Hello& hello,const std::wstring& endpoint) noexcept {
 bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return;
 try{runtime.hello=hello;runtime.endpoint=endpoint;Handle thread{CreateThread(nullptr,0,worker,nullptr,0,nullptr)};if(!thread.valid())runtime.started.store(false);}catch(...){runtime.started.store(false);}
}
}
