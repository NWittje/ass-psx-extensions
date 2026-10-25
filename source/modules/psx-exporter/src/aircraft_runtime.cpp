#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <jni.h>
#include <psx_exporter/aircraft_runtime.hpp>
#include <psx/concurrency/triple_buffer.hpp>
#include <psx/windows/overlapped.hpp>
#include <array>
#include <atomic>
#include <cstring>
namespace {
namespace aircraft=psx::aircraft_sync;
using psx::concurrency::TripleBuffer;
struct Runtime {aircraft::Hello hello{};std::wstring endpoint;std::atomic<std::uint32_t> groups{};std::atomic<bool> ready{},started{};std::atomic<std::uint64_t> state{};std::array<TripleBuffer<aircraft::Sample>,6> samples;} runtime;
std::int64_t ticks() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
using Operation=psx::windows::Overlapped;
bool send(HANDLE pipe,aircraft::Frame& frame) noexcept {
 Operation op;if(!op.ov.hEvent)return false;std::size_t offset=0;const auto deadline=GetTickCount64()+1000;
 while(offset<frame.size){if(!op.begin(pipe,true,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset)))return false;if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.ov.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}if(!op.done)return false;offset+=op.done;}return true;
}
struct Peer {bool subscribed{};std::uint32_t pid{},sequence{};std::uint64_t owner{},capture_generation{};bool capture{};};
bool receive(void* context,const aircraft::Header& h,const std::uint8_t* data,std::size_t size) {
 auto& p=*static_cast<Peer*>(context);if(!h.sequence||h.sequence<=p.sequence)return false;p.sequence=h.sequence;
 if(!p.subscribed){aircraft::Subscribe s;if(h.kind!=aircraft::Kind::subscribe||!aircraft::decode(data,size,s)||s.instance!=runtime.hello.instance||s.qpc_frequency!=runtime.hello.qpc_frequency||s.pid!=p.pid)return false;p.owner=s.owner;p.subscribed=true;return true;}
 if(h.kind==aircraft::Kind::capture){aircraft::Capture c;if(!aircraft::decode(data,size,c)||c.owner!=p.owner||c.generation<p.capture_generation||(c.generation==p.capture_generation&&p.capture!=(c.enabled!=0)))return false;p.capture_generation=c.generation;p.capture=c.enabled!=0;return true;}
 return false; // Exporter sampling is read-only; group leases are held by the plugin.
}
bool service(HANDLE pipe) noexcept {
 Peer peer;ULONG server{};if(!GetNamedPipeServerProcessId(pipe,&server)||!server)return false;peer.pid=server;
 aircraft::Frame output;std::uint32_t sequence=1;runtime.hello.groups=runtime.groups.load();if(!aircraft::encode(runtime.hello,sequence++,output)||!send(pipe,output))return false;
 Operation read,write;std::array<std::uint8_t,2048> bytes{};aircraft::Framer framer;if(!read.ov.hEvent||!write.ov.hEvent||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
 bool okay=true,writing=false;std::size_t offset=0;unsigned lane=0;std::uint64_t state_version=~0ULL;auto progress=GetTickCount64();const auto started=progress;const auto floor=ticks();
 while(okay){
  if(runtime.hello.groups!=runtime.groups.load()){okay=false;break;}
  if(!read.pending||WaitForSingleObject(read.ov.hEvent,0)==WAIT_OBJECT_0){if(!read.finish(pipe)||!read.done||!framer.feed(bytes.data(),read.done,receive,&peer)){okay=false;break;}if(!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size()))){okay=false;break;}}
  if((!peer.subscribed&&GetTickCount64()-started>=1000)||(writing&&GetTickCount64()-progress>=1000)){okay=false;break;}
  if(writing&&(!write.pending||WaitForSingleObject(write.ov.hEvent,0)==WAIT_OBJECT_0)){if(!write.finish(pipe)||!write.done){okay=false;break;}offset+=write.done;progress=GetTickCount64();if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}}else writing=false;}
  if(!writing&&peer.subscribed){bool have=false;const auto state=runtime.state.load(std::memory_order_acquire);if(state!=state_version){aircraft::Status status;status.producer=runtime.hello.instance;status.lifecycle=state>>1;const auto loading=(state&1)!=0;status.groups=loading?0:runtime.groups.load();status.reason=loading?aircraft::Reason::loading:aircraft::Reason::none;have=aircraft::encode(status,sequence++,output);state_version=state;}
   for(unsigned turn=0;!have&&turn<6;++turn){const auto g=lane++%6;aircraft::Sample s;const auto current=runtime.state.load(std::memory_order_acquire);if(runtime.samples[g].take(s)&&s.sample_start_qpc>=floor&&(current&1)==0&&s.lifecycle==(current>>1)&&(runtime.hello.groups&(1u<<g))&&ticks()-s.sample_end_qpc<static_cast<std::int64_t>(runtime.hello.qpc_frequency)&&runtime.state.load(std::memory_order_acquire)==current)have=aircraft::encode(s,sequence++,output);}if(have){offset=0;writing=true;progress=GetTickCount64();if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}if(!write.pending)continue;}}
  HANDLE events[]={read.ov.hEvent,write.ov.hEvent};if(!writing)ResetEvent(write.ov.hEvent);if(WaitForMultipleObjects(2,events,FALSE,2)==WAIT_FAILED){okay=false;break;}
 }
 read.cancel(pipe);write.cancel(pipe);return okay;
}
DWORD WINAPI worker(void*) noexcept {for(;;){HANDLE pipe=CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);if(pipe!=INVALID_HANDLE_VALUE){try{service(pipe);}catch(...){}CloseHandle(pipe);}Sleep(250);}}
}
namespace psx_exporter {
void start_aircraft(const psx::aircraft_sync::Hello& hello,const std::wstring& endpoint) noexcept {bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return;try{runtime.hello=hello;runtime.endpoint=endpoint;runtime.ready.store(true,std::memory_order_release);HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);else {runtime.ready.store(false);runtime.started.store(false);}}catch(...){runtime.ready.store(false);runtime.groups.store(0);runtime.started.store(false);}}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_BridgeNative_aircraftAbiVersion(JNIEnv*,jclass){return aircraft::aircraft_jni_abi;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_aircraftCapabilities(JNIEnv*,jclass,jint groups){runtime.groups.store(static_cast<std::uint32_t>(groups)&aircraft::all_groups);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_aircraftState(JNIEnv*,jclass,jlong lifecycle,jboolean loading){const auto epoch=static_cast<std::uint64_t>(lifecycle);if(!epoch)return;if(epoch>(~0ULL>>1))return;runtime.state.store((epoch<<1)|(loading?1u:0u),std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_BridgeNative_aircraftPublish(JNIEnv*,jclass,jint group,jint family,jint variant,jint flags,jint valid,jlong sequence,jlong lifecycle,jlong start,jlong version,jlong frame,
 jfloat v0,jfloat v1,jfloat v2,jfloat v3,jfloat v4,jfloat v5,jfloat v6,jfloat v7,jfloat v8,jfloat v9,jfloat v10,jfloat v11,jfloat v12,jfloat v13,jfloat v14,jfloat v15,jfloat v16,jfloat v17,jfloat v18,jfloat v19,jfloat v20,jfloat v21,jfloat v22,jfloat v23,jfloat v24,jfloat v25,jfloat v26,jfloat v27,jfloat v28,jfloat v29,jfloat v30,jfloat v31){
 if(group<0||group>=6||family<1||family>3||variant<0||!runtime.ready.load(std::memory_order_acquire)||(runtime.groups.load()&(1u<<group))==0)return;
 aircraft::Sample s;s.producer=runtime.hello.instance;s.lifecycle=static_cast<std::uint64_t>(lifecycle);s.configuration=(static_cast<std::uint64_t>(family)<<32)|static_cast<std::uint32_t>(variant);s.sequence=static_cast<std::uint64_t>(sequence);s.writer_version=static_cast<std::uint64_t>(version);s.frame_sequence=static_cast<std::uint64_t>(frame);s.sample_start_qpc=start;s.sample_end_qpc=ticks();s.group=static_cast<aircraft::Group>(group);s.flags=static_cast<std::uint32_t>(flags)|(frame?aircraft::frame_known:0);s.valid_mask=static_cast<std::uint32_t>(valid)&aircraft::component_mask(s.group);s.approximation_mask=group==3?0x3e0u:group==4?15u:0;
 s.values={v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,v17,v18,v19,v20,v21,v22,v23,v24,v25,v26,v27,v28,v29,v30,v31};
 for(std::size_t i=0;i<32;++i)if(s.valid_mask&(1u<<i)){s.component_qpc[i]=s.sample_end_qpc;s.component_sequence[i]=s.sequence;}runtime.samples[static_cast<std::size_t>(group)].put(s);
}
