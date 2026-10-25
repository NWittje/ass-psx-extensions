#include <psx/situation/fmc_service.hpp>
#include <psx/situation/fmc_native_service.hpp>
#include <jni.h>
#include <jvmti.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <memory>
namespace {namespace s=psx::situation;namespace n=psx::non_normal;namespace f=psx::fmc;namespace w=f::wire;namespace nr=f::native_records;
enum State:unsigned {idle,queued,copying,posted,validating,executing,complete,cancelled};
// One existing command service owns r1 and r2. Negotiation is fixed for each
// authenticated connection; uncertain mutations remain retained, never replayed.
struct Source {f::Compatibility compatibility;std::uint64_t lifecycle{},generation{};std::int64_t observed{};};
struct NativeSource {nr::Compatibility compatibility;std::uint64_t lifecycle{},generation{};std::int64_t observed{};};
const nr::Profile& native_profile(){static const auto value=nr::qualified_profile();return value;}
struct Runtime {
 std::atomic<bool> started{},connected{};std::atomic<unsigned> state{},capabilities{};
 std::atomic<std::uint64_t> lifecycle{},token{},connection{};std::uint64_t pending_connection{},producer{};std::atomic<s::Reason> cancellation{s::Reason::disconnected};n::ProcessIdentity identity{};
 w::Request pending{};w::Result answer{};std::vector<std::uint8_t> input,output;
 std::uint64_t artifact_operation{},artifact_session{};n::ProcessIdentity artifact_client{};
 s::Hash source_hash{};std::atomic<bool> hash_ready{};
 std::atomic<std::shared_ptr<const Source>> source;
 std::atomic<std::shared_ptr<const NativeSource>> native_source;
 unsigned pending_revision=2;std::shared_ptr<const NativeSource> pending_native;
 struct Receipt {bool used{};n::ProcessIdentity client{};std::uint64_t session{},operation{};w::Result answer;unsigned revision{};};
 std::array<Receipt,64> receipts{};std::size_t next{};
 struct Floor {bool used{};n::ProcessIdentity client{};std::uint64_t session{},operation{};};
 std::array<Floor,16> floors{};
} rt;
std::shared_ptr<const Source> current_source(){
 auto value=rt.source.load(std::memory_order_acquire);const auto now=n::qpc();const auto freq=n::qpc_frequency();
 return value&&value->lifecycle==rt.lifecycle.load()&&value->lifecycle&&value->observed>0&&now>=value->observed&&now-value->observed<static_cast<std::int64_t>(freq*3)?value:nullptr;
}
std::shared_ptr<const NativeSource> current_native_source(){
 auto value=rt.native_source.load(std::memory_order_acquire);const auto now=n::qpc();const auto freq=n::qpc_frequency();
 return value&&value->lifecycle==rt.lifecycle.load()&&value->lifecycle&&value->observed>0&&now>=value->observed&&now-value->observed<static_cast<std::int64_t>(freq*3)?value:nullptr;
}
bool native_selection(const w::Request& r){auto source=current_native_source();return source&&r.generation&&r.generation==source->generation;}
w::Result result(const w::Request& r,s::Phase phase,s::Reason reason,unsigned revision=2){
 w::Result value;auto& a=value.common;a.server=rt.identity;a.session=r.session;a.operation=r.operation;a.phase=phase;a.reason=reason;a.capabilities=rt.capabilities.load();a.lifecycle=rt.lifecycle.load();
 if(rt.hash_ready.load(std::memory_order_acquire))a.source_jar_hash=rt.source_hash;
 if(revision==3){if(auto source=current_native_source()){a.capabilities|=w::capture_memory|w::recall_memory|w::native_records;value.generation=source->generation;value.validity=w::compatibility;value.profile=source->compatibility.profile;}}
 else if(auto source=current_source()){a.capabilities|=w::capture_memory;value.generation=source->generation;value.model=static_cast<std::uint32_t>(source->compatibility.model);value.validity=w::compatibility;value.profile=source->compatibility.profile;value.navigation_database=source->compatibility.navigation_database;value.model_configuration=source->compatibility.model_configuration;}
 return value;
}
Runtime::Receipt* receipt(const w::Request& r){for(auto& h:rt.receipts)if(h.used&&h.client==r.client&&h.session==r.session&&h.operation==r.operation)return &h;return nullptr;}
Runtime::Floor* floor(const w::Request& r){for(auto& x:rt.floors)if(x.used&&x.client==r.client&&x.session==r.session)return &x;for(auto& x:rt.floors)if(!x.used){x={true,r.client,r.session,0};return &x;}return nullptr;}
void remember(const w::Request& r,const w::Result& a,unsigned revision=2){auto* h=receipt(r);if(!h)h=&rt.receipts[rt.next++%rt.receipts.size()];*h={true,r.client,r.session,r.operation,a,revision};}
void expire_queued(){
 auto state=rt.state.load(std::memory_order_acquire);
 if((state==queued||state==posted)&&(rt.pending.command==w::Command::capture_situation||rt.pending.command==w::Command::recall_situation)&&n::qpc()>=rt.pending.deadline_qpc){
  rt.cancellation=s::Reason::expired;
  rt.state.compare_exchange_strong(state,cancelled,std::memory_order_acq_rel);
 }
}
void harvest(){auto state=rt.state.load(std::memory_order_acquire);if(state!=complete&&state!=cancelled)return;auto answer=state==complete?rt.answer:result(rt.pending,s::Phase::rejected,rt.cancellation.load(),rt.pending_revision);remember(rt.pending,answer,rt.pending_revision);rt.input.clear();rt.pending_native.reset();rt.state.store(idle,std::memory_order_release);}
void disconnect(){rt.connected.store(false,std::memory_order_release);rt.connection.fetch_add(1);rt.cancellation=s::Reason::disconnected;unsigned state=rt.state.load();while(state==queued||state==posted){if(rt.state.compare_exchange_weak(state,cancelled))break;}}
bool decode(const s::Frame& frame,unsigned revision,w::Request& r,std::uint32_t& sequence){
 if(revision>=2)return w::decode({frame.bytes.data(),frame.size},r,sequence,static_cast<std::uint16_t>(revision));
 s::Request old;if(!s::decode({frame.bytes.data(),frame.size},old,sequence))return false;
 r.command=static_cast<w::Command>(old.command);r.client=old.client;r.target=old.target;r.session=old.session;r.operation=old.operation;r.lifecycle=old.lifecycle;r.frequency=old.frequency;r.issued_qpc=old.issued_qpc;r.deadline_qpc=old.deadline_qpc;r.offset=old.offset;r.total=old.total;r.data=std::move(old.data);return true;
}
bool encode(w::Result value,unsigned revision,std::uint32_t sequence,s::Frame& frame){
 if(revision>=2)return w::encode(value,sequence,frame,static_cast<std::uint16_t>(revision));
 auto& a=value.common;if(a.evidence&w::complete_checkpoint){a.phase=s::Phase::unavailable;a.reason=s::Reason::unsupported;a.offset=0;a.total=0;a.data.clear();}a.capabilities&=15;a.evidence&=15;if(static_cast<unsigned>(a.reason)>11)a.reason=s::Reason::unsupported;if(static_cast<unsigned>(a.phase)>6)a.phase=s::Phase::indeterminate;return s::encode(a,sequence,frame);
}
void service(HANDLE pipe){
 n::ProcessIdentity peer{};if(!n::authenticate(pipe,true,{},peer))return;rt.connected=true;std::uint64_t session{};std::uint32_t incoming{},outgoing{};unsigned revision{};auto last=n::qpc();const auto frequency=n::qpc_frequency();
 auto answer=[&](const w::Request& request,s::Phase phase,s::Reason reason){return result(request,phase,reason,revision);};
 w::Request stage{};std::vector<std::uint8_t> staged;bool staging=false;
 while(rt.connected.load()){
  expire_queued();harvest();if(staging&&(stage.lifecycle!=rt.lifecycle.load()||n::qpc()>=stage.deadline_qpc)){staging=false;staged.clear();}
  s::Frame frame;if(!s::read_frame(pipe,frame,20)){if(GetLastError()!=ERROR_NO_DATA)break;if(!n::connected(pipe)||n::qpc()-last>=static_cast<std::int64_t>(frequency*5))break;continue;}
  unsigned observed=unsigned(frame.bytes[4])|(unsigned(frame.bytes[5])<<8);if(observed!=1&&observed!=2&&observed!=3)break;if(revision&&revision!=observed)break;
  w::Request r;std::uint32_t sequence{};if(!decode(frame,observed,r,sequence)||sequence<=incoming||r.client!=peer||r.target!=rt.identity||r.frequency!=frequency||!r.session||!r.operation||(session&&r.session!=session))break;
  // Only read-only discovery may choose a revision on a new connection.
  if(!revision&&observed>=2&&r.command!=w::Command::status)break;revision=observed;incoming=sequence;session=r.session;last=n::qpc();auto a=answer(r,s::Phase::rejected,s::Reason::malformed);
  bool valid_time=r.issued_qpc>0&&r.issued_qpc<=last&&r.deadline_qpc>last&&r.deadline_qpc-r.issued_qpc<=static_cast<std::int64_t>(frequency*30);
  if(!valid_time)a.common.reason=s::Reason::expired;
  else if(r.command==w::Command::status||r.command==w::Command::heartbeat){a=answer(r,a.common.capabilities?s::Phase::observed:s::Phase::unavailable,a.common.capabilities?s::Reason::none:s::Reason::binding);}
  else if(r.command==w::Command::lookup||r.command==w::Command::fetch){
   auto* h=receipt(r);if(h&&h->revision==revision)a=h->answer;else if(rt.state.load()!=idle&&rt.pending.client==r.client&&rt.pending.session==r.session&&rt.pending.operation==r.operation&&rt.pending_revision==revision){
    const bool entered_expired=rt.state.load(std::memory_order_acquire)==executing&&(rt.pending.command==w::Command::capture_situation||rt.pending.command==w::Command::recall_situation)&&n::qpc()>=rt.pending.deadline_qpc;
    a=answer(r,entered_expired?s::Phase::indeterminate:s::Phase::accepted,entered_expired?s::Reason::expired:s::Reason::none);
   }else a.common.reason=s::Reason::not_retained;
   if(r.command==w::Command::fetch){if(rt.state.load(std::memory_order_acquire)==idle&&h&&a.common.phase==s::Phase::saved_verified&&rt.artifact_client==r.client&&rt.artifact_session==r.session&&rt.artifact_operation==r.operation&&r.total==rt.output.size()&&r.offset<r.total){
    a.common.offset=r.offset;auto count=std::min(revision>=2?w::chunk_bytes:s::chunk_bytes,rt.output.size()-r.offset);a.common.data.assign(rt.output.begin()+r.offset,rt.output.begin()+r.offset+count);
   }else a=answer(r,s::Phase::rejected,s::Reason::not_retained);}
  }
  else if(auto* retained=receipt(r)){if(retained->revision==revision)a=retained->answer;else a.common.reason=s::Reason::not_retained;}
  else if(r.command==w::Command::legacy_fmc_recall||(revision!=3&&r.command==w::Command::recall)){
   // No mutation is queued until the entire native recall adapter qualifies.
   a=answer(r,s::Phase::unavailable,r.command==w::Command::recall?static_cast<s::Reason>(w::Reason::profile):s::Reason::unsupported);remember(r,a,revision);staging=false;staged.clear();
  }
  else if(revision==3&&(r.command==w::Command::capture_situation||r.command==w::Command::recall_situation)){a.common.reason=s::Reason::unsupported;}
  else if(!r.lifecycle||r.lifecycle!=rt.lifecycle.load()){a.common.reason=s::Reason::binding;}
  else if(rt.state.load()!=idle){a.common.reason=s::Reason::busy;}
  else if(revision==3&&(r.command==w::Command::stage||r.command==w::Command::capture||r.command==w::Command::recall)&&!native_selection(r)){
   a.common.reason=static_cast<s::Reason>(w::Reason::profile);staging=false;staged.clear();
  }
  else if(r.command==w::Command::stage){
   auto* h=floor(r);if(!staging){if(!h||r.operation<=h->operation||r.offset!=0||r.data.empty()||!r.total)a.common.reason=s::Reason::identity;else {h->operation=r.operation;stage=r;stage.data.clear();staged.clear();staged.reserve(r.total);staging=true;}}
   if(staging){if(stage.client!=r.client||stage.session!=r.session||stage.operation!=r.operation||stage.lifecycle!=r.lifecycle||stage.generation!=r.generation||stage.total!=r.total||r.offset!=staged.size()||r.data.empty()){staging=false;staged.clear();a.common.reason=s::Reason::identity;}
    else {staged.insert(staged.end(),r.data.begin(),r.data.end());a=answer(r,s::Phase::accepted,s::Reason::none);a.common.total=stage.total;a.common.offset=static_cast<std::uint32_t>(staged.size());}}
  }
  else if(r.command==w::Command::capture_situation||r.command==w::Command::recall_situation||r.command==w::Command::capture||r.command==w::Command::recall){
   auto* h=floor(r);bool allowed=false;bool source_capture=r.command==w::Command::capture;auto source=current_source();auto native_source=revision==3?current_native_source():nullptr;
   if(r.command==w::Command::capture_situation||source_capture){allowed=h&&r.operation>h->operation&&!staging&&r.data.empty()&&!r.total;if(allowed)h->operation=r.operation;}
   else {allowed=staging&&stage.operation==r.operation&&stage.lifecycle==r.lifecycle&&stage.generation==r.generation&&staged.size()==stage.total;try{if(allowed){if(revision==3){if(!native_source)throw std::invalid_argument("native profile unavailable");nr::require_compatible(nr::inspect(staged,native_profile().records),native_profile().records,native_source->compatibility);}else s::validate_native(s::inspect(staged));}}catch(...){allowed=false;}}
   if(revision==3){if(!native_source||!r.generation||r.generation!=native_source->generation){allowed=false;a.common.reason=static_cast<s::Reason>(w::Reason::profile);}}
   else if(source_capture&&(!source||!r.generation||r.generation!=source->generation)){allowed=false;a.common.reason=static_cast<s::Reason>(w::Reason::profile);}
   else if(!source_capture&&!(rt.capabilities.load()&(r.command==w::Command::capture_situation?1u:2u))){allowed=false;a.common.reason=s::Reason::binding;}
   if(!allowed){remember(r,a,revision);staging=false;staged.clear();}
   else {rt.pending=r;rt.pending_revision=revision;rt.pending_native=native_source;rt.pending_connection=rt.connection.load();rt.input=(r.command==w::Command::recall_situation||r.command==w::Command::recall)?std::move(staged):std::vector<std::uint8_t>{};staging=false;rt.token.fetch_add(1);a=answer(r,s::Phase::accepted,s::Reason::none);rt.state.store(queued,std::memory_order_release);}
  }
  if(outgoing==UINT32_MAX||!encode(a,revision,++outgoing,frame)||!s::write_frame(pipe,frame))break;
 }
 disconnect();
}
DWORD WINAPI worker(void*)noexcept {for(;;){try{harvest();auto pipe=n::create_server(s::endpoint(rt.identity));if(pipe==INVALID_HANDLE_VALUE){Sleep(250);continue;}if(n::accept(pipe,250))try{service(pipe);}catch(...){disconnect();}DisconnectNamedPipe(pipe);CloseHandle(pipe);}catch(...){disconnect();Sleep(250);}}}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_SituationNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_SituationNative_initialize(JNIEnv*,jclass,jboolean startup){
 if(!startup)return JNI_FALSE;bool expected=false;if(!rt.started.compare_exchange_strong(expected,true))return JNI_TRUE;rt.identity=n::current_process();rt.producer=n::nonce();auto thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!thread){rt.started=false;return JNI_FALSE;}CloseHandle(thread);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_SituationNative_capabilities(JNIEnv*,jclass,jint caps){rt.capabilities.store(static_cast<unsigned>(caps)&15);if(!caps){rt.cancellation=s::Reason::binding;unsigned state=rt.state.load();while(state==queued||state==posted){if(rt.state.compare_exchange_weak(state,cancelled))break;}}}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_SituationNative_sourceHash(JNIEnv* env,jclass,jobject hash){if(!hash||rt.hash_ready.load()||env->GetDirectBufferCapacity(hash)!=32)return;auto* p=env->GetDirectBufferAddress(hash);if(!p)return;std::memcpy(rt.source_hash.data(),p,32);rt.hash_ready.store(true,std::memory_order_release);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_SituationNative_poll(JNIEnv* env,jclass,jobject buffer,jlong life){
 if(life>0)rt.lifecycle.store(static_cast<std::uint64_t>(life));if(!buffer||env->GetDirectBufferCapacity(buffer)<static_cast<jlong>(s::max_artifact+64))return JNI_FALSE;auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)return JNI_FALSE;unsigned expected=queued;if(!rt.state.compare_exchange_strong(expected,copying,std::memory_order_acq_rel))return JNI_FALSE;
 const auto token=rt.token.load();const auto command=static_cast<std::uint32_t>(rt.pending.command),size=static_cast<std::uint32_t>(rt.input.size());std::memset(p,0,64);std::memcpy(p,&token,8);std::memcpy(p+8,&command,4);std::memcpy(p+12,&size,4);std::memcpy(p+16,&rt.pending.operation,8);std::memcpy(p+24,&rt.pending.generation,8);std::memcpy(p+32,&rt.pending_revision,4);std::memcpy(p+40,&rt.pending.lifecycle,8);if(size)std::memcpy(p+64,rt.input.data(),size);rt.state.store(rt.connected.load()&&rt.connection.load()==rt.pending_connection?posted:cancelled,std::memory_order_release);return JNI_TRUE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_SituationNative_begin(JNIEnv*,jclass,jlong token,jlong life){
 if(token<=0||static_cast<std::uint64_t>(token)!=rt.token.load())return JNI_FALSE;unsigned expected=posted;if(!rt.state.compare_exchange_strong(expected,validating,std::memory_order_acq_rel))return JNI_FALSE;
 auto source=current_source();bool qualified=rt.pending.command==w::Command::capture?source&&source->generation==rt.pending.generation:(rt.capabilities.load()&(rt.pending.command==w::Command::capture_situation?1u:2u))!=0;
 bool valid=qualified&&life>0&&static_cast<std::uint64_t>(life)==rt.pending.lifecycle&&rt.lifecycle.load()==rt.pending.lifecycle&&rt.connected.load()&&rt.connection.load()==rt.pending_connection&&n::qpc()<rt.pending.deadline_qpc;
 if(!valid)rt.cancellation=n::qpc()>=rt.pending.deadline_qpc?s::Reason::expired:!rt.connected.load()?s::Reason::disconnected:s::Reason::replaced;rt.state.store(valid?executing:cancelled,std::memory_order_release);return valid?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_SituationNative_complete(JNIEnv* env,jclass,jlong token,jint reason,jint evidence,jlong life,jlong observation,jobject buffer,jint length,jdoubleArray readback){
 if(token<=0||static_cast<std::uint64_t>(token)!=rt.token.load())return;unsigned state=rt.state.load(std::memory_order_acquire);bool before_native=false;
 if(state==posted&&reason!=0){if(!rt.state.compare_exchange_strong(state,executing,std::memory_order_acq_rel))return;before_native=true;}else if(state!=executing)return;
 try{auto value=result(rt.pending,s::Phase::indeterminate,s::Reason::native_exception);auto& a=value.common;a.evidence=static_cast<unsigned>(evidence)&15;a.lifecycle=life>0?static_cast<std::uint64_t>(life):0;a.observation_qpc=observation;
  if(reason>0&&reason<=11){a.reason=static_cast<s::Reason>(reason);if(before_native)a.phase=s::Phase::rejected;}
  else if(reason==0){
   if(rt.pending.command==w::Command::capture_situation){
    if(!(a.evidence&s::native_returned)||!buffer||length<=0||length>static_cast<jint>(s::max_artifact)||env->GetDirectBufferCapacity(buffer)<length)throw std::invalid_argument("native save buffer");auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)throw std::invalid_argument("native save address");
    s::validate_native(s::inspect({p,static_cast<std::size_t>(length)}));rt.output.assign(p,p+length);a.artifact_hash=s::sha256(rt.output);a.total=static_cast<std::uint32_t>(rt.output.size());a.phase=s::Phase::saved_verified;a.reason=s::Reason::none;rt.artifact_client=rt.pending.client;rt.artifact_session=rt.pending.session;rt.artifact_operation=rt.pending.operation;
   }else{
    if(rt.pending.command!=w::Command::recall_situation||!readback||env->GetArrayLength(readback)!=12)throw std::invalid_argument("native recall readback");env->GetDoubleArrayRegion(readback,0,12,a.readback.data());if(env->ExceptionCheck()){env->ExceptionClear();throw std::invalid_argument("native readback JNI");}
    for(auto x:a.readback)if(!std::isfinite(x))throw std::invalid_argument("native readback finite");
    if(a.evidence!=15||a.readback[8]!=0||a.readback[9]!=1||static_cast<int>(a.readback[10])%2!=1||a.readback[11]!=0||!a.lifecycle||a.observation_qpc<=0)throw std::invalid_argument("native recall evidence");a.phase=s::Phase::observed;a.reason=s::Reason::none;a.artifact_hash=s::sha256(rt.input);
   }
  }
  if(a.lifecycle)rt.lifecycle.store(a.lifecycle);rt.answer=std::move(value);
 }catch(...){rt.answer=result(rt.pending,s::Phase::indeterminate,s::Reason::file);}
 rt.state.store(complete,std::memory_order_release);
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_FmcNative_identity(JNIEnv* env,jclass,jobject buffer){
 if(!rt.started.load()||!rt.hash_ready.load(std::memory_order_acquire)||!buffer||env->GetDirectBufferCapacity(buffer)!=32)return JNI_FALSE;auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)return JNI_FALSE;const auto freq=n::qpc_frequency();std::memset(p,0,32);std::memcpy(p,&rt.identity.pid,4);std::memcpy(p+8,&rt.identity.creation_filetime,8);std::memcpy(p+16,&freq,8);std::memcpy(p+24,&rt.producer,8);return JNI_TRUE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_FmcNative_configure(JNIEnv* env,jclass,jobject buffer,jlong generation){
 if(!buffer||generation<=0){rt.source.store(nullptr,std::memory_order_release);return JNI_FALSE;}
 if(!rt.started.load()||!rt.hash_ready.load(std::memory_order_acquire)||env->GetDirectBufferCapacity(buffer)!=104)return JNI_FALSE;auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)return JNI_FALSE;
 try{std::uint32_t model{},reserved{};std::memcpy(&model,p,4);std::memcpy(&reserved,p+4,4);if((model!=1&&model!=2)||reserved)return JNI_FALSE;
  auto source=std::make_shared<Source>();source->compatibility.source_jar=rt.source_hash;source->compatibility.model=static_cast<f::Model>(model);std::memcpy(source->compatibility.profile.data(),p+8,32);std::memcpy(source->compatibility.navigation_database.data(),p+40,32);std::memcpy(source->compatibility.model_configuration.data(),p+72,32);
  for(const auto& hash:{source->compatibility.profile,source->compatibility.navigation_database,source->compatibility.model_configuration})if(std::all_of(hash.begin(),hash.end(),[](auto v){return v==0;}))return JNI_FALSE;
  source->generation=static_cast<std::uint64_t>(generation);source->lifecycle=rt.lifecycle.load();source->observed=n::qpc();rt.source.store(std::move(source),std::memory_order_release);return JNI_TRUE;
 }catch(...){return JNI_FALSE;}
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_FmcNative_complete(JNIEnv* env,jclass,jlong token,jint reason,jlong life,jlong generation,jlong observation,jobject buffer,jint length){
 if(token<=0||static_cast<std::uint64_t>(token)!=rt.token.load()||rt.pending.command!=w::Command::capture)return;
 unsigned state=rt.state.load(std::memory_order_acquire);if(state==posted&&reason!=0){if(!rt.state.compare_exchange_strong(state,executing,std::memory_order_acq_rel))return;}else if(state!=executing)return;
 auto value=result(rt.pending,s::Phase::rejected,static_cast<s::Reason>(w::Reason::profile));auto& a=value.common;
 try{
  if(reason>0&&reason<=15)a.reason=static_cast<s::Reason>(reason);
  else if(reason==0){
   auto source=current_source();if(!source||life<=0||static_cast<std::uint64_t>(life)!=rt.pending.lifecycle||generation<=0||static_cast<std::uint64_t>(generation)!=rt.pending.generation||source->generation!=rt.pending.generation||observation<=0||!buffer||length<=0||length>static_cast<jint>(s::max_artifact)||env->GetDirectBufferCapacity(buffer)<length)throw std::invalid_argument("FMC source replaced");
   auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)throw std::invalid_argument("FMC source storage");auto snapshot=f::inspect({p,static_cast<std::size_t>(length)});f::require_compatible(snapshot,source->compatibility);
   if(!snapshot.complete||snapshot.source_pid!=rt.identity.pid||snapshot.process_birth!=rt.identity.creation_filetime||snapshot.producer!=rt.producer||snapshot.lifecycle!=static_cast<std::uint64_t>(life)||snapshot.generation!=static_cast<std::uint64_t>(generation)||snapshot.frequency!=n::qpc_frequency()||snapshot.observation_qpc!=observation||snapshot.copy_finish_qpc>n::qpc())throw std::invalid_argument("FMC native provenance");
   rt.output.assign(p,p+length);a.artifact_hash=s::sha256(rt.output);a.total=static_cast<std::uint32_t>(rt.output.size());a.lifecycle=snapshot.lifecycle;a.observation_qpc=observation;a.evidence=w::complete_checkpoint;a.phase=s::Phase::saved_verified;a.reason=s::Reason::none;value.restored_state=f::memory_hash(snapshot);value.validity|=w::memory_readback;
   rt.artifact_client=rt.pending.client;rt.artifact_session=rt.pending.session;rt.artifact_operation=rt.pending.operation;
  }
 }catch(...){a.phase=s::Phase::rejected;a.reason=static_cast<s::Reason>(w::Reason::profile);}
 rt.answer=std::move(value);rt.state.store(complete,std::memory_order_release);
}
// Native-record service admission occurs on the ordinary EDT immediately before
// the unchanged adapter action. Once entered, disconnect/expiry cannot replay it.
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_FmcNativeRecordsNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_FmcNativeRecordsNative_configure(JNIEnv* env,jclass,jobject buffer,jlong life,jlong generation){
 if(!buffer||life<=0||generation<=0){rt.native_source.store(nullptr,std::memory_order_release);return JNI_FALSE;}
 try{
  if(!rt.started.load()||!rt.hash_ready.load(std::memory_order_acquire)||rt.source_hash!=native_profile().compatibility.jar||static_cast<std::uint64_t>(life)!=rt.lifecycle.load()||env->GetDirectBufferCapacity(buffer)!=32)return JNI_FALSE;
  auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p||!std::equal(p,p+32,native_profile().compatibility.profile.begin()))return JNI_FALSE;
  auto source=std::make_shared<NativeSource>();source->compatibility=native_profile().compatibility;source->lifecycle=static_cast<std::uint64_t>(life);source->generation=static_cast<std::uint64_t>(generation);source->observed=n::qpc();rt.native_source.store(std::move(source),std::memory_order_release);return JNI_TRUE;
 }catch(...){rt.native_source.store(nullptr,std::memory_order_release);return JNI_FALSE;}
}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_FmcNativeRecordsNative_remainingNanos(JNIEnv*,jclass,jlong token){
 if(token<=0)return 0;unsigned expected=posted;if(!rt.state.compare_exchange_strong(expected,validating,std::memory_order_acq_rel))return 0;
 jlong value{};if(static_cast<std::uint64_t>(token)==rt.token.load()&&rt.pending_revision==3){const auto remaining=rt.pending.deadline_qpc-n::qpc();if(remaining>0)value=static_cast<jlong>(std::min<long double>(30000000000.0L,static_cast<long double>(remaining)*1000000000.0L/n::qpc_frequency()));}
 // Claim the posted slot while reading it: a disconnected predecessor cannot
 // read a later command's deadline while the pipe worker replaces that slot.
 rt.state.store(posted,std::memory_order_release);return value;
}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_FmcNativeRecordsNative_begin(JNIEnv*,jclass,jlong token,jlong life,jlong generation){
 if(token<=0)return JNI_FALSE;
 unsigned expected=posted;if(!rt.state.compare_exchange_strong(expected,validating,std::memory_order_acq_rel))return JNI_FALSE;
 if(static_cast<std::uint64_t>(token)!=rt.token.load()||rt.pending_revision!=3){rt.state.store(posted,std::memory_order_release);return JNI_FALSE;}
 auto source=rt.native_source.load(std::memory_order_acquire);auto pinned=rt.pending_native;
 bool qualified=source&&pinned&&source->compatibility.jar==pinned->compatibility.jar&&source->compatibility.profile==pinned->compatibility.profile&&source->lifecycle==pinned->lifecycle&&source->generation==pinned->generation;
 bool valid=qualified&&life>0&&generation>0&&static_cast<std::uint64_t>(life)==rt.pending.lifecycle&&static_cast<std::uint64_t>(generation)==rt.pending.generation&&rt.lifecycle.load()==rt.pending.lifecycle&&rt.connected.load()&&rt.connection.load()==rt.pending_connection&&n::qpc()<rt.pending.deadline_qpc;
 if(!valid)rt.cancellation=n::qpc()>=rt.pending.deadline_qpc?s::Reason::expired:!rt.connected.load()?s::Reason::disconnected:s::Reason::replaced;
 rt.state.store(valid?executing:cancelled,std::memory_order_release);return valid?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_FmcNativeRecordsNative_complete(JNIEnv* env,jclass,jlong token,jint phase,jint reason,jlong life,jlong generation,jlong observation,jobject buffer,jint length,jboolean paused,jboolean began){
 if(token<=0)return;
 unsigned state=rt.state.load(std::memory_order_acquire);
 bool claimed=false;if(state==posted&&phase==5&&!began){if(!rt.state.compare_exchange_strong(state,executing,std::memory_order_acq_rel))return;claimed=true;}else if(state!=executing)return;
 if(static_cast<std::uint64_t>(token)!=rt.token.load()||rt.pending_revision!=3){if(claimed)rt.state.store(posted,std::memory_order_release);return;}
 auto value=result(rt.pending,s::Phase::indeterminate,s::Reason::native_exception,3);auto& a=value.common;
 try{
  auto source=rt.pending_native;if(!source)throw std::invalid_argument("native selection unavailable");
  a.source_jar_hash=source->compatibility.jar;value.profile=source->compatibility.profile;value.validity=w::compatibility;
  a.lifecycle=life>0?static_cast<std::uint64_t>(life):0;value.generation=generation>0?static_cast<std::uint64_t>(generation):0;a.observation_qpc=observation;
  if(phase==5&&!began){a.phase=s::Phase::rejected;a.reason=reason>0&&reason<=15?static_cast<s::Reason>(reason):s::Reason::binding;}
  else if(phase==6){a.phase=s::Phase::indeterminate;a.reason=s::Reason::native_exception;}
  else {
   if(!began||reason!=0||a.lifecycle!=rt.pending.lifecycle||a.lifecycle!=rt.lifecycle.load()||observation<rt.pending.issued_qpc||observation>n::qpc())throw std::invalid_argument("native completion authority/time");
   if(rt.pending.command==w::Command::capture&&phase==3){
    if(value.generation!=rt.pending.generation||!buffer||length<=0||length>static_cast<jint>(s::max_artifact)||env->GetDirectBufferCapacity(buffer)<length)throw std::invalid_argument("native capture buffer");
    auto* p=static_cast<std::uint8_t*>(env->GetDirectBufferAddress(buffer));if(!p)throw std::invalid_argument("native capture address");
    auto snapshot=nr::inspect({p,static_cast<std::size_t>(length)},native_profile().records);nr::require_compatible(snapshot,native_profile().records,source->compatibility);const auto& provenance=snapshot.metadata.source;
    if(!snapshot.complete||provenance.pid!=rt.identity.pid||provenance.birth!=rt.identity.creation_filetime||provenance.producer!=rt.producer||provenance.lifecycle!=a.lifecycle||provenance.generation!=value.generation||provenance.frequency!=n::qpc_frequency()||!provenance.frame||provenance.observation<rt.pending.issued_qpc||provenance.finish!=observation)throw std::invalid_argument("native capture provenance");
    rt.output.assign(p,p+length);a.artifact_hash=s::sha256(rt.output);a.total=static_cast<std::uint32_t>(rt.output.size());a.evidence=w::complete_checkpoint;a.phase=s::Phase::saved_verified;a.reason=s::Reason::none;
    rt.artifact_client=rt.pending.client;rt.artifact_session=rt.pending.session;rt.artifact_operation=rt.pending.operation;
   }else if(rt.pending.command==w::Command::recall&&phase==4){
    if(!paused||rt.pending.generation>=INT64_MAX||value.generation!=rt.pending.generation+1)throw std::invalid_argument("native initialization/Pause completion");
    a.artifact_hash=s::sha256(rt.input);a.evidence=static_cast<std::uint32_t>(w::native_initialized)|static_cast<std::uint32_t>(s::pause_observed);value.validity|=w::native_completion;a.readback[9]=1;a.phase=s::Phase::observed;a.reason=s::Reason::none;
   }else throw std::invalid_argument("native completion command/phase");
  }
 }catch(...){a.phase=began?s::Phase::indeterminate:s::Phase::rejected;a.reason=static_cast<s::Reason>(w::Reason::profile);a.evidence=0;value.validity&=w::compatibility;}
 try{if(rt.pending.command==w::Command::recall&&began&&generation>0){
  auto current=rt.native_source.load(std::memory_order_acquire);
  if(current&&current->lifecycle==rt.pending.lifecycle&&current->generation==rt.pending.generation){auto successor=std::make_shared<NativeSource>(*current);successor->generation=static_cast<std::uint64_t>(generation);successor->observed=n::qpc();rt.native_source.store(std::move(successor),std::memory_order_release);}
 }}catch(...){rt.native_source.store(nullptr,std::memory_order_release);}
 rt.answer=std::move(value);rt.state.store(complete,std::memory_order_release);
}
// Independent FMC JNI1: preparation/worker binding only. This does not start
// transport, change legacy situation capabilities, or initialize a vendor class.
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_FmcNative_abiVersion(JNIEnv*,jclass){return 1;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_FmcNative_classStates(JNIEnv* env,jclass,jobjectArray owners,jintArray states){
 if(!env||env->ExceptionCheck()||!owners||!states)return JNI_FALSE;
 const auto count=env->GetArrayLength(owners);
 if(env->ExceptionCheck()||count<=0||count>64||env->GetArrayLength(states)!=count||env->ExceptionCheck())return JNI_FALSE;
 JavaVM* vm=nullptr;
 if(env->GetJavaVM(&vm)!=JNI_OK||!vm)return JNI_FALSE;
 jvmtiEnv* inspection=nullptr;
 // Each GetEnv(JVMTI) creates an environment. One bounded batch owns one,
 // requests no optional capabilities/events, and disposes it before returning.
 if(vm->GetEnv(reinterpret_cast<void**>(&inspection),JVMTI_VERSION_1_2)!=JNI_OK||!inspection)return JNI_FALSE;
 jint observed[64]{};bool valid=true;
 for(jsize i=0;i<count&&valid;++i){
  auto owner=static_cast<jclass>(env->GetObjectArrayElement(owners,i));
  if(env->ExceptionCheck()||!owner){valid=false;if(owner)env->DeleteLocalRef(owner);break;}
  valid=inspection->GetClassStatus(owner,&observed[i])==JVMTI_ERROR_NONE;
  env->DeleteLocalRef(owner);
 }
 const auto disposal=inspection->DisposeEnvironment();
 if(!valid||disposal!=JVMTI_ERROR_NONE||env->ExceptionCheck())return JNI_FALSE;
 // No field/method IDs are resolved and no Java method is invoked. A missing,
 // failed or still-initializing owner is reported by its actual status bits;
 // the qualified worker must refuse native binding unless all owners qualify.
 env->SetIntArrayRegion(states,0,count,observed);
 return env->ExceptionCheck()?JNI_FALSE:JNI_TRUE;
}
