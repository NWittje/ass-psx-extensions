#include <psx/non_normal/pipe.hpp>
#include <psx_exporter/reset_runtime.hpp>
#include <jni.h>
#include <atomic>
#include <array>
#include <cstring>
namespace {namespace n=psx::non_normal;
// Worker owns records. Release/acquire state publishes one request to the Java daemon/EDT.
struct Runtime {std::atomic<bool> started{},capable{},connected{};std::atomic<unsigned> state{};
 std::atomic<std::uint64_t> lifecycle{},token{};std::atomic<n::Reason> cancel_reason{n::Reason::disconnected};n::Request pending{};std::uint64_t pending_lifecycle{};n::ProcessIdentity identity{};} runtime;
enum State:unsigned {idle,queued,copying,posted,validating,executing,returned,fault,cancelled};
n::Result result(const n::Request& r,n::Phase phase,n::Reason reason)noexcept {n::Result v;v.server=runtime.identity;v.session=r.session;v.request=r.request;v.command=r.command;v.phase=phase;v.reason=reason;v.capabilities=runtime.capable.load()?n::reset_capability:0;v.observed_qpc=n::qpc();v.revision=runtime.lifecycle.load();return v;}
void invalidate()noexcept {runtime.connected.store(false,std::memory_order_release);unsigned s=runtime.state.load();while(s==queued||s==posted){if(runtime.state.compare_exchange_weak(s,cancelled))break;}}
void service(HANDLE pipe)noexcept {
 n::ProcessIdentity peer{};if(!n::authenticate(pipe,true,{},peer))return;
 runtime.connected.store(true,std::memory_order_release);std::uint64_t session{};std::uint32_t incoming{},outgoing{};
 std::int64_t last=n::qpc();std::uint64_t frequency=n::qpc_frequency(),high_water{};
 struct History {bool used{};n::Request request{};n::Result result{};};std::array<History,64> history{};std::size_t next{};
 auto send=[&](const n::Result& r){n::Frame f;return outgoing!=UINT32_MAX&&n::encode(r,++outgoing,f)&&n::write_frame(pipe,f);};
 while(runtime.connected.load()) {
  auto state=runtime.state.load(std::memory_order_acquire);
  if((state==queued||state==posted)&&n::qpc()>=runtime.pending.deadline_qpc){
   runtime.cancel_reason=n::Reason::expired;
   if(runtime.state.compare_exchange_strong(state,cancelled,std::memory_order_acq_rel))state=cancelled;
  }
  if((state==returned||state==fault||state==cancelled)&&runtime.pending.session==session) {
   const auto phase=state==returned?n::Phase::action_returned:state==fault?n::Phase::indeterminate:n::Phase::rejected;
   const auto reason=state==returned?n::Reason::none:state==fault?n::Reason::native_exception:runtime.cancel_reason.load();
   auto answer=result(runtime.pending,phase,reason);for(auto& h:history)if(h.used&&h.request.request==runtime.pending.request)h.result=answer;
   if(!send(answer))break;runtime.state.store(idle,std::memory_order_release);
  }
  n::Frame frame;if(n::read_frame(pipe,frame,20)) {
   n::Request r;std::uint32_t sequence{};if(!n::decode({frame.bytes.data(),frame.size},r,sequence)||sequence<=incoming||r.client!=peer||r.target!=runtime.identity||r.frequency!=frequency||(session&&r.session!=session))break;
   incoming=sequence;session=r.session;last=n::qpc();
   bool duplicate=false;for(const auto& h:history)if(h.used&&h.request.request==r.request){duplicate=true;if(!send(h.request==r?h.result:result(r,n::Phase::rejected,n::Reason::identity)))runtime.connected=false;break;}if(duplicate)continue;
   if(r.request<=high_water){if(!send(result(r,n::Phase::rejected,n::Reason::identity)))break;continue;}high_water=r.request;
   if(r.issued_qpc>last||r.deadline_qpc<=last){if(!send(result(r,n::Phase::rejected,n::Reason::expired)))break;continue;}
   auto answer=result(r,n::Phase::unavailable,n::Reason::unsupported);
   if(r.command==n::Command::status||r.command==n::Command::heartbeat){answer.phase=runtime.capable.load()?n::Phase::applied:n::Phase::unavailable;answer.reason=runtime.capable.load()?n::Reason::none:n::Reason::binding;}
   else if(r.command==n::Command::reset_malfunctions) {
    if(!runtime.capable.load()||!runtime.lifecycle.load()){answer.reason=n::Reason::binding;}
    else if(runtime.state.load()!=idle){answer.phase=n::Phase::rejected;answer.reason=n::Reason::busy;}
    else {runtime.pending=r;runtime.pending_lifecycle=runtime.lifecycle.load();runtime.cancel_reason=n::Reason::disconnected;runtime.token.fetch_add(1);runtime.state.store(queued,std::memory_order_release);answer.phase=n::Phase::accepted;answer.reason=n::Reason::none;}
   }
   if(r.command!=n::Command::status&&r.command!=n::Command::heartbeat)history[next++%history.size()]={true,r,answer};if(!send(answer))break;
  }else if(GetLastError()!=ERROR_NO_DATA)break;
  if(n::qpc()-last>=static_cast<std::int64_t>(frequency)||!n::connected(pipe))break;
 }
 invalidate();
}
DWORD WINAPI worker(void*)noexcept {for(;;){
 for(;;){unsigned s=runtime.state.load(std::memory_order_acquire);if(s==executing||s==copying||s==validating){Sleep(5);continue;}if(s==queued||s==posted){runtime.state.compare_exchange_strong(s,cancelled);continue;}if(runtime.state.compare_exchange_strong(s,idle,std::memory_order_acq_rel))break;}
 HANDLE pipe=INVALID_HANDLE_VALUE;try{pipe=n::create_server(n::reset_endpoint(runtime.identity));}catch(...){runtime.capable=false;Sleep(250);continue;}if(pipe==INVALID_HANDLE_VALUE){Sleep(250);continue;}
 if(n::accept(pipe,250))service(pipe);invalidate();DisconnectNamedPipe(pipe);CloseHandle(pipe);
 } }
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_ResetNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_ResetNative_initialize(JNIEnv*,jclass,jboolean startup){if(!startup)return JNI_FALSE;bool expected=false;if(!runtime.started.compare_exchange_strong(expected,true))return JNI_TRUE;runtime.identity=n::current_process();HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!thread){runtime.started=false;return JNI_FALSE;}CloseHandle(thread);return JNI_TRUE;}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_ResetNative_capabilities(JNIEnv*,jclass,jboolean available){runtime.capable.store(available!=0,std::memory_order_release);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_ResetNative_poll(JNIEnv* env,jclass,jobject buffer,jlong lifecycle){if(lifecycle>0)runtime.lifecycle.store(static_cast<std::uint64_t>(lifecycle));if(!buffer||env->GetDirectBufferCapacity(buffer)<40)return JNI_FALSE;auto* address=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!address)return JNI_FALSE;unsigned expected=queued;if(!runtime.state.compare_exchange_strong(expected,copying,std::memory_order_acq_rel))return JNI_FALSE;const std::array<std::uint64_t,5> values{runtime.token.load(),runtime.pending.session,runtime.pending.request,static_cast<std::uint64_t>(runtime.pending.deadline_qpc),runtime.pending_lifecycle};std::memcpy(address,values.data(),40);runtime.state.store(runtime.connected.load()?posted:cancelled,std::memory_order_release);return JNI_TRUE;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_ResetNative_begin(JNIEnv*,jclass,jlong token,jlong lifecycle){
 if(token<=0)return JNI_FALSE;unsigned s=posted;if(!runtime.state.compare_exchange_strong(s,validating,std::memory_order_acq_rel))return JNI_FALSE;
 if(static_cast<std::uint64_t>(token)!=runtime.token.load()){runtime.state.store(posted,std::memory_order_release);return JNI_FALSE;}
 if(lifecycle<=0||static_cast<std::uint64_t>(lifecycle)!=runtime.pending_lifecycle||runtime.lifecycle.load()!=runtime.pending_lifecycle||!runtime.connected.load()||!runtime.capable.load()||n::qpc()>=runtime.pending.deadline_qpc){
  runtime.cancel_reason=n::qpc()>=runtime.pending.deadline_qpc?n::Reason::expired:!runtime.connected.load()?n::Reason::disconnected:n::Reason::replaced;
  runtime.state.store(cancelled,std::memory_order_release);return JNI_FALSE;
 }
 runtime.state.store(executing,std::memory_order_release);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_ResetNative_complete(JNIEnv*,jclass,jlong token,jboolean success){if(token>0&&static_cast<std::uint64_t>(token)==runtime.token.load()){unsigned s=executing;if(!runtime.state.compare_exchange_strong(s,success?returned:fault,std::memory_order_acq_rel)&&!success){s=posted;runtime.state.compare_exchange_strong(s,cancelled);}}}
namespace psx_exporter {bool reset_available()noexcept{return runtime.capable.load(std::memory_order_acquire);}}
