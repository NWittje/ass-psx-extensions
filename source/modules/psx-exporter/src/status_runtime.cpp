#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <jni.h>
#include <psx_exporter/status_runtime.hpp>
#include <psx/concurrency/triple_buffer.hpp>
#include <psx/windows/overlapped.hpp>
#include <array>
#include <atomic>
namespace {
namespace s=psx::status;
using psx::concurrency::TripleBuffer;
struct Runtime {s::Hello hello{};std::wstring endpoint;std::atomic<std::uint32_t> capabilities{};std::atomic<s::Reason> reason{s::Reason::unsupported};std::atomic<bool> ready{},started{};TripleBuffer<s::Snapshot> snapshots;} runtime;
std::int64_t ticks() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
using Operation=psx::windows::Overlapped;
bool send(HANDLE pipe,s::Frame& frame) noexcept {
 Operation op;if(!op.ov.hEvent)return false;std::size_t offset=0;const auto deadline=GetTickCount64()+1000;
 while(offset<frame.size){if(!op.begin(pipe,true,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset)))return false;if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.ov.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}if(!op.done)return false;offset+=op.done;}return true;
}
struct Peer {bool subscribed{};std::uint32_t pid{},sequence{};std::uint64_t connection{};std::int64_t last_received{},floor{};};
bool receive(void* context,const s::Header& h,const std::uint8_t* data,std::size_t size){
 auto& p=*static_cast<Peer*>(context);if(!h.sequence||h.sequence<=p.sequence)return false;p.sequence=h.sequence;
 if(!p.subscribed){s::Subscribe sub;if(h.kind!=s::Kind::subscribe||!s::decode(data,size,sub)||sub.instance!=runtime.hello.instance||sub.pid!=p.pid||sub.qpc_frequency!=runtime.hello.qpc_frequency)return false;p.connection=sub.connection_id;p.subscribed=true;p.floor=ticks();}
 else {s::Heartbeat hb;if(h.kind!=s::Kind::heartbeat||!s::decode(data,size,hb)||hb.producer!=runtime.hello.instance||hb.connection_id!=p.connection)return false;}
 p.last_received=ticks();return true;
}
bool service(HANDLE pipe) noexcept {
 Peer peer;ULONG server{};if(!GetNamedPipeServerProcessId(pipe,&server)||!server)return false;peer.pid=server;peer.last_received=ticks();
 s::Frame output;std::uint32_t sequence=1;auto hello=runtime.hello;hello.capabilities=runtime.capabilities.load();if(!s::encode(hello,sequence++,output)||!send(pipe,output))return false;
 Operation read,write;std::array<std::uint8_t,s::max_frame> bytes{};s::Framer framer;if(!read.ov.hEvent||!write.ov.hEvent||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
 bool okay=true,writing=false;std::size_t offset=0;auto progress=GetTickCount64();std::int64_t last_heartbeat{};
 while(okay){const auto now=ticks();
  if(now-peer.last_received>=static_cast<std::int64_t>(hello.qpc_frequency*5)||(writing&&GetTickCount64()-progress>=1000)){okay=false;break;}
  if(!read.pending||WaitForSingleObject(read.ov.hEvent,0)==WAIT_OBJECT_0){if(!read.finish(pipe)||!read.done||!framer.feed(bytes.data(),read.done,receive,&peer)){okay=false;break;}if(!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size()))){okay=false;break;}}
  if(writing&&(!write.pending||WaitForSingleObject(write.ov.hEvent,0)==WAIT_OBJECT_0)){if(!write.finish(pipe)||!write.done){okay=false;break;}offset+=write.done;progress=GetTickCount64();if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}}else writing=false;}
  if(!writing&&peer.subscribed){bool have=false;s::Snapshot snapshot;
   if(runtime.snapshots.take(snapshot)&&snapshot.observed_qpc>peer.floor&&runtime.capabilities.load()){snapshot.connection_id=peer.connection;have=s::encode(snapshot,sequence++,output);}
   if(!have&&now-last_heartbeat>=static_cast<std::int64_t>(hello.qpc_frequency/4)){s::Heartbeat hb;hb.producer=hello.instance;hb.connection_id=peer.connection;hb.generated_qpc=now;hb.capabilities=runtime.capabilities.load();hb.reason=runtime.reason.load();have=s::encode(hb,sequence++,output);last_heartbeat=now;}
   if(have){offset=0;writing=true;progress=GetTickCount64();if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}}
  }
  HANDLE events[]={read.ov.hEvent,write.ov.hEvent};if(!writing)ResetEvent(write.ov.hEvent);if(WaitForMultipleObjects(2,events,FALSE,2)==WAIT_FAILED){okay=false;break;}
 }
 read.cancel(pipe);write.cancel(pipe);return okay;
}
DWORD WINAPI worker(void*) noexcept {for(;;){HANDLE pipe=CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);if(pipe!=INVALID_HANDLE_VALUE){service(pipe);CloseHandle(pipe);}Sleep(250);}}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_StatusNative_abiVersion(JNIEnv*,jclass){return s::status_jni_abi;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_StatusNative_intervalTicks(JNIEnv*,jclass){LARGE_INTEGER f{};return QueryPerformanceFrequency(&f)?f.QuadPart/4:0;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_StatusNative_capabilities(JNIEnv*,jclass,jint mask){const auto caps=static_cast<std::uint32_t>(mask)&s::fatal_state;runtime.reason.store(caps?s::Reason::none:s::Reason::unsupported);runtime.capabilities.store(caps,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_StatusNative_failed(JNIEnv*,jclass){runtime.reason.store(s::Reason::native_fault);runtime.capabilities.store(0,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_StatusNative_publish(JNIEnv*,jclass,jlong sequence,jlong lifecycle,jlong observed,jint flags){
 if(!runtime.ready.load(std::memory_order_acquire)||!runtime.capabilities.load(std::memory_order_acquire)||sequence<=0||lifecycle<=0||observed<=0)return;
 s::Snapshot snapshot;snapshot.producer=runtime.hello.instance;snapshot.role=s::Role::exporter;snapshot.sequence=static_cast<std::uint64_t>(sequence);snapshot.lifecycle=static_cast<std::uint64_t>(lifecycle);snapshot.observed_qpc=observed;snapshot.flags=static_cast<std::uint32_t>(flags);snapshot.reason=(flags&s::qualified)?s::Reason::none:s::Reason::loading;runtime.snapshots.put(snapshot);
}
namespace psx_exporter {
void start_status(const psx::status::Hello& hello,const std::wstring& endpoint) noexcept {bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return;try{runtime.hello=hello;runtime.endpoint=endpoint;runtime.ready.store(true,std::memory_order_release);HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);else {runtime.ready.store(false);runtime.started.store(false);}}catch(...){runtime.ready.store(false);runtime.capabilities.store(0);runtime.started.store(false);}}
}
