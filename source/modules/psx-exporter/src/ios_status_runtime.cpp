#include <psx/instructor/ios_status_native.hpp>
#include <jni.h>
#include <atomic>

namespace {
namespace s=psx::instructor::ios_status_native;
namespace n=psx::non_normal;
struct Group {
 std::uint64_t sequence{},lifecycle{},frame{};std::int64_t observed{},finished{};
 int model{-1};std::uint32_t valid{};std::array<float,32> values{};
};
// A slot can be read only after an exclusive Ready->Reading claim. Concurrent
// callbacks never write a reader-owned slot, including when old samples are dropped.
struct Mailbox {
 std::array<Group,3> slots{};std::array<std::atomic<unsigned>,3> states{};
 bool put(const Group&value)noexcept{
  for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<3;++i){unsigned expected=pass?2u:0u;
   if(states[i].compare_exchange_strong(expected,1,std::memory_order_acq_rel)){
    slots[i]=value;states[i].store(2,std::memory_order_release);return true;
   }}return false;
 }
 bool take(Group&value)noexcept{
  bool found=false;for(unsigned i=0;i<3;++i){unsigned expected=2;
   if(states[i].compare_exchange_strong(expected,3,std::memory_order_acq_rel)){
    if(!found||slots[i].sequence>value.sequence){value=slots[i];found=true;}
    states[i].store(0,std::memory_order_release);
   }}return found;
 }
};
struct Runtime {
 std::atomic<bool> started{},loading{true};std::atomic<unsigned> capabilities{};
 std::atomic<std::uint64_t> lifecycle{};n::ProcessIdentity identity{};
 std::array<Mailbox,4> mailboxes{};std::array<Group,4> current{};std::uint64_t revision{};
} runtime;
s::Snapshot snapshot(){
 s::Snapshot answer;answer.server=runtime.identity;answer.frequency=n::qpc_frequency();answer.lifecycle=runtime.lifecycle.load(std::memory_order_acquire);
 const bool loading=runtime.loading.load(std::memory_order_acquire);const unsigned capabilities=runtime.capabilities.load(std::memory_order_acquire);
 answer.flags=loading?s::loading:capabilities?0:s::source_unavailable;
 for(unsigned g=0;g<4;++g){Group next{};if(runtime.mailboxes[g].take(next)){
  if(next.lifecycle==answer.lifecycle&&next.sequence>runtime.current[g].sequence){runtime.current[g]=next;if(runtime.revision!=UINT64_MAX)++runtime.revision;}
 }
 const Group&current=runtime.current[g];
 if(answer.flags||!(capabilities&(1u<<g))||current.lifecycle!=answer.lifecycle||!current.sequence)continue;
 answer.groups|=1u<<g;answer.observed_qpc[g]=current.observed;answer.group_sequence[g]=current.sequence;answer.frame[g]=current.frame;
 if(current.finished>answer.copy_finished_qpc)answer.copy_finished_qpc=current.finished;
 if(g==1)answer.engine_model=current.model;
 for(unsigned i=0;i<s::count[g];++i){const unsigned slot=s::first[g]+i;answer.values[slot]=current.values[i];if(current.valid&(1u<<i))answer.values_valid|=1u<<slot;}
 }
 answer.sequence=runtime.revision;return answer;
}
void service(HANDLE pipe){
 n::ProcessIdentity peer{};if(!n::authenticate(pipe,true,{},peer))return;
 std::uint64_t session{},high{};std::uint32_t incoming{},outgoing{};auto last=n::qpc();const auto frequency=n::qpc_frequency();
 for(;;){
  s::Frame frame;if(s::read_frame(pipe,frame,20)){
   s::Request request;std::uint32_t sequence{};const auto now=n::qpc();
   if(!s::decode(frame,request,sequence)||sequence<=incoming||request.request<=high||request.client!=peer||request.target!=runtime.identity||
      request.frequency!=frequency||(session&&request.session!=session)||request.issued>now||request.deadline<=now||
      request.deadline-request.issued>static_cast<std::int64_t>(frequency*2))break;
   incoming=sequence;session=request.session;high=request.request;last=now;
   auto answer=snapshot();answer.session=session;answer.request=high;
   if(outgoing==UINT32_MAX||!s::write_frame(pipe,s::encode(answer,++outgoing)))break;
  }else if(GetLastError()!=ERROR_NO_DATA)break;
  if(n::qpc()-last>=static_cast<std::int64_t>(frequency*2)||!n::connected(pipe))break;
 }
}
DWORD WINAPI worker(void*)noexcept{
 for(;;){HANDLE pipe=INVALID_HANDLE_VALUE;try{pipe=n::create_server(s::endpoint(runtime.identity));if(pipe!=INVALID_HANDLE_VALUE&&n::accept(pipe,250))service(pipe);}catch(...){}
  if(pipe!=INVALID_HANDLE_VALUE){DisconnectNamedPipe(pipe);CloseHandle(pipe);}else Sleep(250);
 }
}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_StatusControlsNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_StatusControlsNative_frequency(JNIEnv*,jclass){return static_cast<jlong>(n::qpc_frequency());}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_StatusControlsNative_qpc(JNIEnv*,jclass){return n::qpc();}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_StatusControlsNative_initialize(JNIEnv*,jclass,jboolean startup){
 if(!startup)return JNI_FALSE;bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return JNI_TRUE;
 runtime.identity=n::current_process();HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
 if(!thread){runtime.started.store(false);return JNI_FALSE;}CloseHandle(thread);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_StatusControlsNative_state(JNIEnv*,jclass,jint capabilities,jlong lifecycle,jboolean loading){
 runtime.capabilities.store(static_cast<unsigned>(capabilities)&15u,std::memory_order_release);
 runtime.loading.store(loading!=0,std::memory_order_release);runtime.lifecycle.store(lifecycle>0?static_cast<std::uint64_t>(lifecycle):0,std::memory_order_release);
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_StatusControlsNative_publish(JNIEnv*,jclass,jint group,jint model,jint valid,jlong sequence,jlong lifecycle,jlong observed,jlong frame,
 jfloat v0,jfloat v1,jfloat v2,jfloat v3,jfloat v4,jfloat v5,jfloat v6,jfloat v7,jfloat v8,jfloat v9,jfloat v10,jfloat v11,jfloat v12,jfloat v13,jfloat v14,jfloat v15,
 jfloat v16,jfloat v17,jfloat v18,jfloat v19,jfloat v20,jfloat v21,jfloat v22,jfloat v23,jfloat v24,jfloat v25,jfloat v26,jfloat v27,jfloat v28,jfloat v29,jfloat v30,jfloat v31){
 if(group<0||group>=4||sequence<=0||lifecycle<=0||observed<=0||frame<=0||!(runtime.capabilities.load(std::memory_order_acquire)&(1u<<group)))return;
 if(group==1&&(model<0||model>2))return;
 Group value;value.model=model;value.sequence=static_cast<std::uint64_t>(sequence);value.lifecycle=static_cast<std::uint64_t>(lifecycle);
 value.frame=static_cast<std::uint64_t>(frame);value.observed=observed;value.finished=n::qpc();if(value.finished<observed)return;
 value.values={v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15,v16,v17,v18,v19,v20,v21,v22,v23,v24,v25,v26,v27,v28,v29,v30,v31};
 for(unsigned i=0;i<s::count[static_cast<unsigned>(group)];++i)if((static_cast<unsigned>(valid)&(1u<<i))&&s::value_valid(s::first[static_cast<unsigned>(group)]+i,value.values[i]))value.valid|=1u<<i;
 if(group==1&&model==0)value.valid&=15u; // CF6 EPR is not a qualified display value.
 runtime.mailboxes[static_cast<unsigned>(group)].put(value);
}
