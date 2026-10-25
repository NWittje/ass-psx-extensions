#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <jni.h>
#include <psx_exporter/environment_runtime.hpp>
#include <psx/environment/presentation.hpp>
#include <psx/concurrency/triple_buffer.hpp>
#include <psx/windows/overlapped.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
namespace {
namespace environment=psx::environment;
namespace presentation=psx::environment::presentation;
using psx::concurrency::TripleBuffer;
struct PendingContext {presentation::Context context{};std::uint64_t lifecycle{},version{};std::int64_t sample_start{};DWORD thread{};};
struct ChangeSlot {std::atomic<std::uint64_t> version{},revision{},lifecycle{};std::atomic<std::int64_t> completed{};std::atomic<std::uint32_t> reason{};};
struct Runtime {environment::Hello hello{};std::wstring endpoint,endpoint2,endpoint3,endpoint4;std::atomic<std::uint32_t> groups{};std::atomic<bool> ready{},started{},context_ready{},change_ready{},local_ready{};std::atomic<std::uint64_t> state{};TripleBuffer<presentation::Source> samples;std::atomic_flag context_lock=ATOMIC_FLAG_INIT,change_lock=ATOMIC_FLAG_INIT,local_lock=ATOMIC_FLAG_INIT;presentation::LocalVisibility local_visibility{};PendingContext context;presentation::WeatherChange change{},previous_change{};std::array<ChangeSlot,32> changes;std::atomic<std::uint64_t> change_revision{};std::uint64_t cloud_hash{},configuration{1},lifecycle{};} runtime;
std::int64_t ticks() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
double degrees(double radians) noexcept {double v=std::fmod(radians*57.2957795130823208768,360.0);return v<0?v+360:v;}
void convert(environment::Sample& s) noexcept {
 auto& v=s.values;auto& c=s.configured;const auto mask=s.valid_mask;
 std::uint64_t hash=1469598103934665603ULL;
 for(unsigned i=5;i<14;i++){std::uint64_t bits{};std::memcpy(&bits,&v[i],8);hash=(hash^bits)*1099511628211ULL;}
 hash=(hash^(mask&environment::cloud_fields))*1099511628211ULL;
 if(runtime.lifecycle!=s.lifecycle||runtime.cloud_hash!=hash){if(runtime.lifecycle)++runtime.configuration;runtime.lifecycle=s.lifecycle;runtime.cloud_hash=hash;}
 s.configuration=runtime.configuration;
 if(mask&2)v[1]*=57.2957795130823208768;if(mask&4)v[2]*=57.2957795130823208768;
 if(mask&8)v[3]*=.3048;if(mask&16)v[4]=v[3]-v[4]*.3048;
 for(unsigned i=5;i<14;i+=3){if(mask&(7u<<i)){v[i]/=8;v[i+1]=v[i+1]*.3048+v[4];v[i+2]=v[i+2]*.3048+v[4];}if(s.configured_mask&(7u<<i)){c[i]/=8;c[i+1]=c[i+1]*.3048+v[4];c[i+2]=c[i+2]*.3048+v[4];}}
 if(mask&(1u<<14))v[14]=degrees(v[14]);if(mask&(1u<<15))v[15]*=.51444444444444444444;if(mask&(1u<<17))v[17]*=100;
 if(s.configured_mask&(1u<<15))c[15]*=.51444444444444444444;if(s.configured_mask&(1u<<17))c[17]*=33.863887;if(s.configured_mask&(1u<<22))c[22]*=.3048;
 for(unsigned i=0;i<32;i++){if(!(s.valid_mask&(1u<<i)))v[i]=0;if(!(s.configured_mask&(1u<<i)))c[i]=0;}
}
using Operation=psx::windows::Overlapped;
bool send(HANDLE pipe,environment::Frame& frame) noexcept {
 Operation op;if(!op.ov.hEvent)return false;std::size_t offset=0;const auto deadline=GetTickCount64()+1000;
 while(offset<frame.size){if(!op.begin(pipe,true,frame.bytes.data()+offset,static_cast<DWORD>(frame.size-offset)))return false;if(op.pending){const auto now=GetTickCount64();if(now>=deadline||WaitForSingleObject(op.ov.hEvent,static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0){op.cancel(pipe);return false;}if(!op.finish(pipe)){op.cancel(pipe);return false;}}if(!op.done)return false;offset+=op.done;}return true;
}
struct Peer {bool subscribed{};std::uint32_t pid{},sequence{};std::uint64_t owner{},capture_generation{};bool capture{};};
bool receive(void* context,const environment::Header& h,const std::uint8_t* data,std::size_t size) {
 auto& p=*static_cast<Peer*>(context);if(!h.sequence||h.sequence<=p.sequence)return false;p.sequence=h.sequence;
 if(!p.subscribed){environment::Subscribe s;if(h.kind!=environment::Kind::subscribe||!environment::decode(data,size,s)||s.instance!=runtime.hello.instance||s.qpc_frequency!=runtime.hello.qpc_frequency||s.pid!=p.pid)return false;p.owner=s.owner;p.subscribed=true;return true;}
 if(h.kind==environment::Kind::capture){environment::Capture c;if(!environment::decode(data,size,c)||c.owner!=p.owner||c.generation<p.capture_generation||(c.generation==p.capture_generation&&p.capture!=(c.enabled!=0)))return false;p.capture_generation=c.generation;p.capture=c.enabled!=0;return true;}
 return false; // Exporter sampling is read-only; group leases are held by the plugin.
}
bool receive2(void* context,const presentation::Header& h,const std::uint8_t* data,std::size_t size) {
 if(h.kind!=presentation::Kind::subscribe&&h.kind!=presentation::Kind::capture)return false;
 environment::Header base{static_cast<environment::Kind>(h.kind),h.payload_size,h.sequence};return receive(context,base,data,size);
}
bool service(HANDLE pipe,bool v2,bool v3,bool v4) noexcept {
 Peer peer;ULONG server{};if(!GetNamedPipeServerProcessId(pipe,&server)||!server)return false;peer.pid=server;
 environment::Frame output;std::uint32_t sequence=1;runtime.hello.groups=runtime.groups.load();
 if(!(v2?presentation::encode(presentation::Hello{runtime.hello,presentation::source_context|(v3?presentation::manual_weather:0u)|(v4&&runtime.local_ready.load()?presentation::local_visibility_target:0u)},sequence++,output):environment::encode(runtime.hello,sequence++,output))||!send(pipe,output))return false;
 Operation read,write;std::array<std::uint8_t,2048> bytes{};environment::Framer framer;presentation::Framer framer2;if(!read.ov.hEvent||!write.ov.hEvent||!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size())))return false;
 bool okay=true,writing=false;std::size_t offset=0;std::uint64_t state_version=~0ULL;auto progress=GetTickCount64();const auto started=progress;const auto floor=ticks();
 while(okay){
  if(runtime.hello.groups!=runtime.groups.load()||sequence==UINT32_MAX){okay=false;break;}
  if(!read.pending||WaitForSingleObject(read.ov.hEvent,0)==WAIT_OBJECT_0){if(!read.finish(pipe)||!read.done||!(v2?framer2.feed(bytes.data(),read.done,receive2,&peer):framer.feed(bytes.data(),read.done,receive,&peer))){okay=false;break;}if(!read.begin(pipe,false,bytes.data(),static_cast<DWORD>(bytes.size()))){okay=false;break;}}
  if((!peer.subscribed&&GetTickCount64()-started>=1000)||(writing&&GetTickCount64()-progress>=1000)){okay=false;break;}
  if(writing&&(!write.pending||WaitForSingleObject(write.ov.hEvent,0)==WAIT_OBJECT_0)){if(!write.finish(pipe)||!write.done){okay=false;break;}offset+=write.done;progress=GetTickCount64();if(offset<output.size){if(!write.begin(pipe,true,output.bytes.data()+offset,static_cast<DWORD>(output.size-offset))){okay=false;break;}}else writing=false;}
  if(!writing&&peer.subscribed){bool have=false;const auto state=runtime.state.load(std::memory_order_acquire);if(state!=state_version){environment::Status status;status.producer=runtime.hello.instance;status.lifecycle=state>>1;const auto loading=(state&1)!=0;status.groups=loading?0:runtime.groups.load();status.reason=loading?environment::Reason::loading:environment::Reason::none;have=v2?presentation::encode(status,sequence++,output):environment::encode(status,sequence++,output);state_version=state;}
   if(!have){presentation::Source bundle;const auto current=runtime.state.load(std::memory_order_acquire);if(runtime.samples.take(bundle)){auto& s=bundle.sample;const auto age=ticks()-s.sample_end_qpc;if(s.sample_start_qpc>=floor&&(current&1)==0&&s.lifecycle==(current>>1)&&age>=0&&age<static_cast<std::int64_t>(runtime.hello.qpc_frequency)&&runtime.state.load(std::memory_order_acquire)==current){convert(s);if(!v3)bundle.change={};if(!v4)bundle.local_visibility={};if(!presentation::valid(bundle)){bundle.context={};bundle.context.source_sequence=s.sequence;}have=v2?presentation::encode(bundle,sequence++,output):environment::encode(s,sequence++,output);}}}if(have){offset=0;writing=true;progress=GetTickCount64();if(!write.begin(pipe,true,output.bytes.data(),static_cast<DWORD>(output.size))){okay=false;break;}if(!write.pending)continue;}}
  HANDLE events[]={read.ov.hEvent,write.ov.hEvent};if(!writing)ResetEvent(write.ov.hEvent);if(WaitForMultipleObjects(2,events,FALSE,2)==WAIT_FAILED){okay=false;break;}
 }
 read.cancel(pipe);write.cancel(pipe);return okay;
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_EnvironmentVisibilityNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentVisibilityNative_capabilities(JNIEnv*,jclass,jboolean value){runtime.local_ready.store(value!=JNI_FALSE,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentVisibilityNative_record(JNIEnv*,jclass,jint metres,jlong lifecycle){
 if(!runtime.local_ready.load(std::memory_order_acquire)||metres<0||metres>9999||lifecycle<=0||runtime.local_lock.test_and_set(std::memory_order_acquire))return;
 runtime.local_visibility={static_cast<std::uint64_t>(lifecycle),ticks(),static_cast<double>(metres),1};runtime.local_lock.clear(std::memory_order_release);
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_EnvironmentNative_abiVersion(JNIEnv*,jclass){return environment::environment_jni_abi;}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_WeatherChangeNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeatherChangeNative_capabilities(JNIEnv*,jclass,jboolean available){runtime.change_ready.store(available!=JNI_FALSE,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeatherChangeNative_mark(JNIEnv*,jclass,jint reason,jlong lifecycle,jint slot){
 if(!runtime.change_ready.load(std::memory_order_acquire)||reason<1||reason>3||lifecycle<=0||slot<0||slot>=32)return;
 auto& row=runtime.changes[static_cast<unsigned>(slot)];row.version.fetch_add(1,std::memory_order_acq_rel);
 const auto revision=runtime.change_revision.fetch_add(1,std::memory_order_acq_rel)+1;
 row.revision.store(revision,std::memory_order_relaxed);row.lifecycle.store(static_cast<std::uint64_t>(lifecycle),std::memory_order_relaxed);
 row.reason.store(static_cast<std::uint32_t>(reason),std::memory_order_relaxed);row.completed.store(ticks(),std::memory_order_relaxed);
 row.version.fetch_add(1,std::memory_order_release);if(!revision)runtime.change_ready.store(false,std::memory_order_release);
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_WeatherChangeNative_consumed(JNIEnv*,jclass,jlong lifecycle,jlong phase_start){
 if(!runtime.change_ready.load(std::memory_order_acquire)||lifecycle<=0||phase_start<=0||runtime.change_lock.test_and_set(std::memory_order_acquire))return JNI_FALSE;
 struct Unlock {~Unlock(){runtime.change_lock.clear(std::memory_order_release);}} unlock;
 presentation::WeatherChange candidate=runtime.change;std::uint64_t completed_latest{};
 for(const auto& row:runtime.changes){const auto before=row.version.load(std::memory_order_acquire);if(before&1)return JNI_FALSE;
  presentation::WeatherChange pending;pending.revision=row.revision.load(std::memory_order_relaxed);pending.lifecycle=row.lifecycle.load(std::memory_order_relaxed);
  pending.completed_qpc=row.completed.load(std::memory_order_relaxed);pending.reason=row.reason.load(std::memory_order_relaxed);pending.qualified=1;
  std::atomic_thread_fence(std::memory_order_acquire);if(row.version.load(std::memory_order_acquire)!=before)return JNI_FALSE;
  if(pending.completed_qpc<=phase_start)completed_latest=(std::max)(completed_latest,pending.revision);
  if(pending.revision>candidate.revision&&pending.completed_qpc<=phase_start&&(pending.lifecycle==static_cast<std::uint64_t>(lifecycle)||pending.reason==3))candidate=pending;
 }
 if(candidate.revision>runtime.change.revision){runtime.previous_change=runtime.change;runtime.change=candidate;runtime.change.lifecycle=static_cast<std::uint64_t>(lifecycle);}
 return completed_latest&&completed_latest==runtime.change_revision.load(std::memory_order_acquire)?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_EnvironmentNative_intervalTicks(JNIEnv*,jclass){LARGE_INTEGER f{};return QueryPerformanceFrequency(&f)?f.QuadPart/5:0;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentNative_capabilities(JNIEnv*,jclass,jint groups){runtime.groups.store(static_cast<std::uint32_t>(groups)&environment::all_groups);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentNative_state(JNIEnv*,jclass,jlong lifecycle,jboolean loading){const auto epoch=static_cast<std::uint64_t>(lifecycle);if(!epoch||epoch>(~0ULL>>1))return;runtime.state.store((epoch<<1)|(loading?1u:0u),std::memory_order_release);}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_EnvironmentContextNative_abiVersion(JNIEnv*,jclass){return presentation::context_jni_abi;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentContextNative_capabilities(JNIEnv*,jclass,jboolean available){runtime.context_ready.store(available!=JNI_FALSE,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentContextNative_publish(JNIEnv*,jclass,jlong sequence,jlong lifecycle,jlong start,jlong version,jlong from,jlong to,jint valid,jint active,jint planet,jint zone,jlong station){
 if(!runtime.context_ready.load(std::memory_order_acquire)||!sequence||!lifecycle||from<start||to<from||!valid||(valid&~15)||runtime.context_lock.test_and_set(std::memory_order_acquire))return;
 PendingContext value;value.context.source_sequence=static_cast<std::uint64_t>(sequence);value.context.observation_start_qpc=from;value.context.observation_end_qpc=to;value.context.valid_mask=static_cast<std::uint32_t>(valid);value.context.active_native_zone=active;value.context.planet_visibility_m=static_cast<std::uint32_t>(planet);value.context.zone1_visibility_m=static_cast<std::uint32_t>(zone);
 const auto bits=static_cast<std::uint64_t>(station);for(unsigned i=0;i<8;++i)value.context.zone1_station[i]=static_cast<char>(bits>>(i*8));
 value.lifecycle=static_cast<std::uint64_t>(lifecycle);value.version=static_cast<std::uint64_t>(version);value.sample_start=start;value.thread=GetCurrentThreadId();runtime.context=value;runtime.context_lock.clear(std::memory_order_release);
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_EnvironmentNative_publish(JNIEnv*,jclass,jint flags,jint valid,jint configured,jlong sequence,jlong lifecycle,jlong start,jlong version,jlong frame,jlong utc,
 jdouble v0,jdouble v1,jdouble v2,jdouble v3,jdouble v4,jdouble v5,jdouble v6,jdouble v7,jdouble v8,jdouble v9,jdouble v10,jdouble v11,jdouble v12,jdouble v13,jdouble v14,jdouble v15,jdouble v16,jdouble v17,jdouble v18,jdouble v19,
 jdouble c0,jdouble c1,jdouble c2,jdouble c3,jdouble c4,jdouble c5,jdouble c6,jdouble c7,jdouble c8,jdouble c9,jdouble c10,jdouble c11,jdouble c12,jdouble c13,jdouble c14,jdouble c15,jdouble c16,jdouble c17,jdouble c18,jdouble c19,jdouble c22){
 if(!runtime.ready.load(std::memory_order_acquire)||!runtime.groups.load())return;
 environment::Sample s;s.producer=runtime.hello.instance;s.lifecycle=static_cast<std::uint64_t>(lifecycle);s.sequence=static_cast<std::uint64_t>(sequence);s.writer_version=static_cast<std::uint64_t>(version);s.frame_sequence=static_cast<std::uint64_t>(frame);s.sample_start_qpc=start;s.sample_end_qpc=ticks();s.utc_millis=utc;s.flags=static_cast<std::uint32_t>(flags);s.valid_mask=static_cast<std::uint32_t>(valid);s.configured_mask=static_cast<std::uint32_t>(configured);
 s.values={v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,v17,v18,v19};s.configured={c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19,0,0,c22};
 presentation::Source bundle;bundle.sample=s;bundle.context.source_sequence=s.sequence;
 if(!runtime.context_lock.test_and_set(std::memory_order_acquire)){
  const auto& p=runtime.context;if(runtime.context_ready.load(std::memory_order_acquire)&&p.context.source_sequence==s.sequence&&p.lifecycle==s.lifecycle&&p.version==s.writer_version&&p.sample_start==s.sample_start_qpc&&p.thread==GetCurrentThreadId()&&p.context.observation_start_qpc>=s.sample_start_qpc&&p.context.observation_end_qpc<=s.sample_end_qpc)bundle.context=p.context;
  runtime.context={};runtime.context_lock.clear(std::memory_order_release);
 }
 if(runtime.change_ready.load(std::memory_order_acquire)){
  // Contention rejects this copy, not the independently qualified capability.
  // Publishing a transient unqualified sample would retire healthy weather.
  if(runtime.change_lock.test_and_set(std::memory_order_acquire))return;
  auto change=runtime.change.completed_qpc<=s.sample_start_qpc?runtime.change:runtime.previous_change;
  if(change.completed_qpc<=s.sample_start_qpc){change.qualified=1;bundle.change=change;}
  runtime.change_lock.clear(std::memory_order_release);
 }
 if((s.valid_mask&environment::bit(environment::Field::visibility_m))&&runtime.local_ready.load(std::memory_order_acquire)&&!runtime.local_lock.test_and_set(std::memory_order_acquire)){
  const auto local=runtime.local_visibility;runtime.local_lock.clear(std::memory_order_release);
  if(local.qualified&&local.lifecycle==s.lifecycle&&local.observed_qpc<=s.sample_end_qpc&&(!bundle.change.revision||bundle.change.completed_qpc<=local.observed_qpc))bundle.local_visibility=local;
 }
 runtime.samples.put(bundle);
}
DWORD WINAPI worker(void*) noexcept {for(;;){bool v4=!runtime.endpoint4.empty(),v3=!runtime.endpoint3.empty(),v2=v3||!runtime.endpoint2.empty();
 HANDLE pipe=v4?CreateFileW(runtime.endpoint4.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr):INVALID_HANDLE_VALUE;
 if(pipe==INVALID_HANDLE_VALUE){v4=false;pipe=v3?CreateFileW(runtime.endpoint3.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr):INVALID_HANDLE_VALUE;}
 if(pipe==INVALID_HANDLE_VALUE){v3=false;pipe=runtime.endpoint2.empty()?INVALID_HANDLE_VALUE:CreateFileW(runtime.endpoint2.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);}
 if(pipe==INVALID_HANDLE_VALUE){v2=false;pipe=CreateFileW(runtime.endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);}
 if(pipe!=INVALID_HANDLE_VALUE){try{service(pipe,v2,v3,v4);}catch(...){}CloseHandle(pipe);}Sleep(250);}}

}
namespace psx_exporter {
void start_environment(const psx::environment::Hello& hello,const std::wstring& endpoint) noexcept {bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return;try{runtime.hello=hello;runtime.endpoint=endpoint;const std::wstring prefix=L"\\\\.\\pipe\\psx-ios.environment.exporter.v1.";if(endpoint.starts_with(prefix))runtime.endpoint2=L"\\\\.\\pipe\\psx-ios.environment.exporter.v2."+endpoint.substr(prefix.size());if(!runtime.endpoint2.empty()){runtime.endpoint3=runtime.endpoint2;auto at=runtime.endpoint3.find(L".v2.");if(at!=std::wstring::npos)runtime.endpoint3.replace(at,4,L".v3.");}runtime.endpoint4=runtime.endpoint3;{auto at=runtime.endpoint4.find(L".v3.");if(at!=std::wstring::npos)runtime.endpoint4.replace(at,4,L".v4.");}runtime.ready.store(true,std::memory_order_release);HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(t)CloseHandle(t);else {runtime.ready.store(false);runtime.started.store(false);}}catch(...){runtime.ready.store(false);runtime.groups.store(0);runtime.started.store(false);}}
}
