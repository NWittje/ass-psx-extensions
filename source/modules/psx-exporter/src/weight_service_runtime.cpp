#include <psx/instructor/weight_native.hpp>
#include <jni.h>
#include <atomic>
#include <array>
#include <mutex>
namespace {
namespace w=psx::instructor::weight_native;
namespace n=psx::non_normal;
enum State:unsigned {idle,queued,copying,posted,validating,executing,returned,fault,cancelled};
struct Runtime {
 std::atomic<bool> started{},connected{},loading{true},uncertain{};
 std::atomic<unsigned> state{};
 std::atomic<std::uint64_t> lifecycle{},token{};
 std::atomic<n::Reason> reason{n::Reason::disconnected};
 n::ProcessIdentity identity{};
 w::Request pending{};
 std::uint64_t pending_lifecycle{};
 std::mutex sample_mutex;
 w::Result sample;
} runtime;
void advance_lifecycle(std::uint64_t life)noexcept {
 auto current=runtime.lifecycle.load(std::memory_order_acquire);
 while(life>current&&!runtime.lifecycle.compare_exchange_weak(current,life,std::memory_order_acq_rel)){}
}
w::Result result(const w::Request&r,n::Phase phase,n::Reason reason) {
 w::Result v;{std::lock_guard lock(runtime.sample_mutex);v=runtime.sample;}
 const auto life=runtime.lifecycle.load(std::memory_order_acquire);
 if(v.lifecycle!=life)v.capabilities=0;
 v.command=r.command;v.server=runtime.identity;v.session=r.session;v.request=r.request;v.phase=phase;v.reason=reason;v.lifecycle=life;
 const auto now=n::qpc(),freq=std::int64_t(n::qpc_frequency());
 if(runtime.loading.load()||v.sampled_qpc<=0||now<v.sampled_qpc||now-v.sampled_qpc>freq*2)v.capabilities=0;
 return v;
}
void invalidate()noexcept {runtime.connected.store(false,std::memory_order_release);unsigned s=runtime.state.load();while(s==queued||s==posted)if(runtime.state.compare_exchange_weak(s,cancelled))break;}
void service(HANDLE pipe) {
 n::ProcessIdentity peer{};if(!n::authenticate(pipe,true,{},peer))return;
 runtime.connected=true;std::uint64_t session{},high{};std::uint32_t incoming{},outgoing{};
 auto last=n::qpc();const auto frequency=n::qpc_frequency();bool reported_uncertain=false;
 struct History {bool used{};w::Request request{};w::Result result{};};std::array<History,64> history{};std::size_t next{};
 auto send=[&](const w::Result&r){return outgoing!=UINT32_MAX&&n::write_frame(pipe,w::encode(r,++outgoing));};
 while(runtime.connected.load()){
  auto state=runtime.state.load(std::memory_order_acquire);
  if(state==executing&&runtime.uncertain.load()&&!reported_uncertain&&runtime.pending.session==session){if(!send(result(runtime.pending,n::Phase::indeterminate,n::Reason::expired)))break;reported_uncertain=true;}
  if((state==queued||state==posted)&&n::qpc()>=runtime.pending.deadline){runtime.reason=n::Reason::expired;if(runtime.state.compare_exchange_strong(state,cancelled,std::memory_order_acq_rel))state=cancelled;}
  if((state==returned||state==fault||state==cancelled)&&runtime.pending.session==session){
   auto answer=result(runtime.pending,state==returned?n::Phase::action_returned:state==fault?n::Phase::indeterminate:n::Phase::rejected,state==returned?n::Reason::none:state==fault?n::Reason::native_exception:runtime.reason.load());
   for(auto&h:history)if(h.used&&h.request.request==runtime.pending.request)h.result=answer;
   if(!send(answer))break;runtime.state.store(idle,std::memory_order_release);
  }
  n::Frame frame;
  if(n::read_frame(pipe,frame,20)){
   w::Request r;std::uint32_t sequence{};
   if(!w::decode(frame,r,sequence)||sequence<=incoming||r.client!=peer||r.target!=runtime.identity||r.frequency!=frequency||(session&&r.session!=session))break;
   incoming=sequence;session=r.session;last=n::qpc();
   bool duplicate=false;for(const auto&h:history)if(h.used&&h.request.request==r.request){duplicate=true;if(!send(h.request==r?h.result:result(r,n::Phase::rejected,n::Reason::identity)))runtime.connected=false;break;}if(duplicate)continue;
   if(r.request<=high){if(!send(result(r,n::Phase::rejected,n::Reason::identity)))break;continue;}high=r.request;
   auto answer=result(r,n::Phase::unavailable,n::Reason::binding);
   if(r.issued>last||r.deadline<=last||r.deadline-r.issued>std::int64_t(frequency*5)){answer.phase=n::Phase::rejected;answer.reason=n::Reason::expired;}
   else if(r.command==w::Command::status){answer.phase=answer.capabilities?n::Phase::applied:n::Phase::unavailable;answer.reason=answer.capabilities?n::Reason::none:n::Reason::binding;}
   else {
    const auto capability=1u<<(unsigned(r.command)-1);
    if(!(answer.capabilities&capability)||!r.lifecycle||r.lifecycle!=runtime.lifecycle.load()){answer.reason=n::Reason::binding;}
    else if(runtime.state.load()!=idle){answer.phase=n::Phase::rejected;answer.reason=n::Reason::busy;}
    else {runtime.pending=r;runtime.pending_lifecycle=r.lifecycle;runtime.reason=n::Reason::disconnected;runtime.token.fetch_add(1);runtime.uncertain=false;reported_uncertain=false;runtime.state.store(queued,std::memory_order_release);answer.phase=n::Phase::accepted;answer.reason=n::Reason::none;}
   }
   if(r.command!=w::Command::status)history[next++%history.size()]={true,r,answer};
   if(!send(answer))break;
  }else if(GetLastError()!=ERROR_NO_DATA)break;
  if(n::qpc()-last>=std::int64_t(frequency*2)||!n::connected(pipe))break;
 }
 invalidate();
}
DWORD WINAPI worker(void*)noexcept {
 for(;;){
  for(;;){unsigned s=runtime.state.load(std::memory_order_acquire);if(s==executing||s==copying||s==validating){Sleep(5);continue;}if(s==queued||s==posted){runtime.state.compare_exchange_strong(s,cancelled);continue;}if(runtime.state.compare_exchange_strong(s,idle,std::memory_order_acq_rel))break;}
  HANDLE pipe=INVALID_HANDLE_VALUE;
  try{pipe=n::create_server(w::endpoint(runtime.identity));if(pipe!=INVALID_HANDLE_VALUE&&n::accept(pipe,250))service(pipe);}catch(...){/* Optional service failures do not replace simulator behavior. */}
  invalidate();if(pipe!=INVALID_HANDLE_VALUE){DisconnectNamedPipe(pipe);CloseHandle(pipe);}else Sleep(250);
 }
}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_WeightNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_WeightNative_initialize(JNIEnv*,jclass,jboolean startup){
 if(!startup)return JNI_FALSE;bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return JNI_TRUE;
 runtime.identity=n::current_process();HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!t){runtime.started=false;return JNI_FALSE;}CloseHandle(t);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeightNative_sample(JNIEnv*,jclass,jint caps,jlong life,jdouble cg,jdouble max,jint min,jboolean loading){
 if(life>0)advance_lifecycle(std::uint64_t(life));
 const auto current=runtime.lifecycle.load(std::memory_order_acquire);const bool coherent=life>0&&std::uint64_t(life)==current;
 if(coherent)runtime.loading.store(loading!=0);
 std::lock_guard lock(runtime.sample_mutex);runtime.sample.lifecycle=life>0?std::uint64_t(life):0;runtime.sample.capabilities=coherent?(unsigned(caps)&63):0;runtime.sample.sampled_qpc=n::qpc();runtime.sample.zfw_cg=cg;runtime.sample.fuel_max_lb=max;runtime.sample.zfw_min_lb=std::uint32_t(min);
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_WeightNative_poll(JNIEnv*env,jclass,jobject buffer,jlong life){
 if(life>0)advance_lifecycle(std::uint64_t(life));if(!buffer||env->GetDirectBufferCapacity(buffer)<32)return JNI_FALSE;
 auto*address=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!address)return JNI_FALSE;
 unsigned expected=queued;if(!runtime.state.compare_exchange_strong(expected,copying,std::memory_order_acq_rel))return JNI_FALSE;
 const std::array<std::uint64_t,3> values{runtime.token.load(),runtime.pending_lifecycle,std::uint64_t(runtime.pending.command)};
 std::memcpy(address,values.data(),24);std::memcpy(address+24,&runtime.pending.fuel_lb,8);
 runtime.state.store(runtime.connected.load()?posted:cancelled,std::memory_order_release);return JNI_TRUE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_WeightNative_begin(JNIEnv*,jclass,jlong token,jlong life){
 unsigned expected=posted;if(token<=0||!runtime.state.compare_exchange_strong(expected,validating,std::memory_order_acq_rel))return JNI_FALSE;
 if(std::uint64_t(token)!=runtime.token.load()){runtime.state.store(posted,std::memory_order_release);return JNI_FALSE;}
 auto status=result(runtime.pending,n::Phase::accepted,n::Reason::none);auto capability=1u<<(unsigned(runtime.pending.command)-1);
 if(life<=0||std::uint64_t(life)!=runtime.pending_lifecycle||runtime.lifecycle.load()!=runtime.pending_lifecycle||!runtime.connected.load()||!(status.capabilities&capability)||n::qpc()>=runtime.pending.deadline){
  runtime.reason=n::qpc()>=runtime.pending.deadline?n::Reason::expired:!runtime.connected.load()?n::Reason::disconnected:n::Reason::replaced;runtime.state.store(cancelled,std::memory_order_release);return JNI_FALSE;
 }
 runtime.state.store(executing,std::memory_order_release);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeightNative_complete(JNIEnv*,jclass,jlong token,jboolean success){
 if(token<=0||std::uint64_t(token)!=runtime.token.load())return;unsigned expected=executing;
 if(!runtime.state.compare_exchange_strong(expected,(success&&runtime.pending_lifecycle==runtime.lifecycle.load()&&!runtime.loading.load()&&!runtime.uncertain.load())?returned:fault,std::memory_order_acq_rel)&&!success){expected=posted;runtime.state.compare_exchange_strong(expected,cancelled);}
}

extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeightNative_reject(JNIEnv*,jclass,jlong token){
 if(token<=0||std::uint64_t(token)!=runtime.token.load())return;unsigned expected=posted;
 runtime.reason=n::Reason::busy;runtime.state.compare_exchange_strong(expected,cancelled,std::memory_order_acq_rel);
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_WeightNative_actionAbiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_WeightNative_uncertain(JNIEnv*,jclass,jlong token){if(token>0&&std::uint64_t(token)==runtime.token.load()&&runtime.state.load()==executing)runtime.uncertain=true;}
