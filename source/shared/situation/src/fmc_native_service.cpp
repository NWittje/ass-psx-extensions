#include <psx/situation/fmc_native_service.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace psx::fmc::native_records {
namespace s=psx::situation;namespace n=psx::non_normal;namespace w=wire;
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
bool nonzero(const Hash& hash){return std::any_of(hash.begin(),hash.end(),[](auto v){return v!=0;});}
w::Result unavailable(w::Result r){r.common.phase=s::Phase::unavailable;r.common.reason=s::Reason::unsupported;r.common.evidence=0;r.validity=0;return r;}
bool terminal(s::Phase phase){return phase!=s::Phase::accepted&&phase!=s::Phase::action_returned;}
}
Client::Client(s::ProcessIdentity target,Profile profile):target_(target),self_(n::current_process()),profile_(std::move(profile)),session_(n::nonce()),frequency_(n::qpc_frequency()){
 require(target.pid&&target.creation_filetime&&nonzero(profile_.compatibility.jar)&&nonzero(profile_.compatibility.profile)&&!profile_.records.keys.empty()&&profile_.records.keys.size()<=max_records,"Native FMC client selection/profile");
}
Client::~Client(){disconnect();}
void Client::disconnect()noexcept{if(pipe_!=INVALID_HANDLE_VALUE){CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}sent_=received_=0;}
void Client::open(){
 disconnect();const auto name=s::endpoint(target_);const auto end=GetTickCount64()+1500;
 // The single authenticated service must retire the previous pipe before it
 // accepts reconnect. Waiting here repeats discovery only, never a mutation.
 do{
  pipe_=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
  if(pipe_!=INVALID_HANDLE_VALUE)break;const auto error=GetLastError();const auto now=GetTickCount64();
  if(now>=end||(error!=ERROR_PIPE_BUSY&&error!=ERROR_FILE_NOT_FOUND))break;
  if(error==ERROR_PIPE_BUSY)WaitNamedPipeW(name.c_str(),static_cast<DWORD>(std::min<std::uint64_t>(100,end-now)));else Sleep(10);
 }while(true);
 s::ProcessIdentity actual{};
 if(pipe_==INVALID_HANDLE_VALUE||!n::authenticate(pipe_,false,target_,actual)){disconnect();throw std::runtime_error("Native FMC service/process identity unavailable");}
}
w::Request Client::request(w::Command command,std::uint64_t operation,std::uint64_t life,std::uint64_t generation){
 w::Request r;r.command=command;r.client=self_;r.target=target_;r.session=session_;r.operation=operation;r.lifecycle=life;r.generation=generation;r.frequency=frequency_;r.issued_qpc=n::qpc();r.deadline_qpc=r.issued_qpc+static_cast<std::int64_t>(frequency_*30);return r;
}
w::Result Client::exchange(const w::Request& q){
 require(pipe_!=INVALID_HANDLE_VALUE,"Native FMC service disconnected");s::Frame frame;
 auto fail=[this](){disconnect();throw std::runtime_error("Native FMC transport outcome uncertain; reconnect and lookup, never resend");};
 bool encoded=false;
 if(version_==1){s::Request r;r.command=static_cast<s::Command>(q.command);r.client=q.client;r.target=q.target;r.session=q.session;r.operation=q.operation;r.lifecycle=q.lifecycle;r.frequency=q.frequency;r.issued_qpc=q.issued_qpc;r.deadline_qpc=q.deadline_qpc;r.offset=q.offset;r.total=q.total;r.data=q.data;encoded=sent_!=UINT32_MAX&&s::encode(r,++sent_,frame);}
 else encoded=sent_!=UINT32_MAX&&w::encode(q,++sent_,frame,version_);
 if(!encoded||!s::write_frame(pipe_,frame)||!s::read_frame(pipe_,frame,1500))fail();
 w::Result r;std::uint32_t sequence{};bool decoded;
 if(version_==1)decoded=s::decode({frame.bytes.data(),frame.size},r.common,sequence);
 else decoded=w::decode({frame.bytes.data(),frame.size},r,sequence,version_);
 if(!decoded||sequence<=received_||r.common.server!=target_||r.common.session!=q.session||r.common.operation!=q.operation)fail();
 received_=sequence;return r;
}
void Client::connect(){
 // Only read-only discovery may reconnect using an older revision. The
 // original session/operation and pending mutation survive every reconnect.
 for(auto version:{w::native_revision,w::revision,std::uint16_t{1}}){
  version_=version;try{open();status();return;}catch(...){disconnect();}
 }
 throw std::runtime_error("Native FMC discovery unavailable");
}
w::Result Client::status(){auto r=exchange(request(w::Command::status,++next_));return native_peer()?r:unavailable(std::move(r));}
bool Client::available(const w::Result& r,w::Capability capability)const{
 return native_peer()&&r.common.phase==s::Phase::observed&&(r.common.capabilities&(w::native_records|capability))==(w::native_records|capability)&&(r.validity&w::compatibility)&&r.common.source_jar_hash==profile_.compatibility.jar&&r.profile==profile_.compatibility.profile&&r.common.lifecycle&&r.generation;
}
void Client::complete(const w::Result& r){
 if(!pending_.operation||r.common.operation!=pending_.operation||!terminal(r.common.phase))return;
 if(r.common.reason==s::Reason::not_retained)return; // An unknown outcome is never permission to repeat.
 if(r.common.phase==s::Phase::observed&&pending_.command==w::Command::recall){
  require(r.common.reason==s::Reason::none&&r.common.lifecycle==pending_.lifecycle&&pending_.generation<INT64_MAX&&r.generation==pending_.generation+1&&r.common.artifact_hash==pending_.artifact,"Native FMC recall completion identity");
  constexpr auto native_proof=static_cast<std::uint32_t>(w::native_initialized)|static_cast<std::uint32_t>(s::pause_observed);
  require((r.common.evidence&native_proof)==native_proof&&(r.validity&(w::compatibility|w::native_completion))==(w::compatibility|w::native_completion)&&r.common.readback[9]==1&&r.common.observation_qpc>=pending_.issued&&r.common.observation_qpc<=n::qpc(),"Native FMC recall lacks initialization/Pause evidence");
  require(r.common.source_jar_hash==profile_.compatibility.jar&&r.profile==profile_.compatibility.profile&&!(r.common.evidence&(static_cast<std::uint32_t>(w::unrelated_preserved)|static_cast<std::uint32_t>(w::rollback_restored)|static_cast<std::uint32_t>(s::zero_rates_observed)))&&!(r.validity&w::preservation)&&!nonzero(r.preserved_state),"Native FMC completion contract mismatch");
 }
 if(pending_.command!=w::Command::capture||r.common.phase!=s::Phase::saved_verified)pending_={};
}
w::Result Client::lookup(std::uint64_t operation){
 require(operation!=0,"Native FMC operation identity");
 if(!native_peer())return unavailable(status());
 auto r=exchange(request(w::Command::lookup,operation));complete(r);return r;
}
w::Result Client::wait(std::uint64_t operation,std::uint32_t timeout){
 require(timeout<=30000,"Native FMC wait bound");auto end=GetTickCount64()+timeout;w::Result r;
 do{r=lookup(operation);if(terminal(r.common.phase))return r;if(GetTickCount64()>=end)return r;Sleep(20);}while(true);
}
w::Result Client::capture(std::vector<std::uint8_t>& artifact,std::uint32_t timeout){
 require(timeout<=30000,"Native FMC wait bound");
 artifact.clear();require(!pending_.operation,"Native FMC operation still unresolved");auto state=status();
 if(!available(state,w::capture_memory))return unavailable(std::move(state));
 auto operation=++next_;last_=operation;auto q=request(w::Command::capture,operation,state.common.lifecycle,state.generation);pending_={w::Command::capture,operation,q.lifecycle,q.generation,q.issued_qpc,{}};
 auto r=exchange(q);if(r.common.phase==s::Phase::accepted||r.common.phase==s::Phase::action_returned)r=wait(operation,timeout);else complete(r);
 if(r.common.phase!=s::Phase::saved_verified)return r;return fetch_capture(operation,artifact);
}
w::Result Client::fetch_capture(std::uint64_t operation,std::vector<std::uint8_t>& artifact){
 artifact.clear();auto r=lookup(operation);if(r.common.phase!=s::Phase::saved_verified)return r;
 require((r.common.evidence&w::complete_checkpoint)&&(r.validity&w::compatibility)&&r.common.total&&r.common.total<=s::max_artifact&&r.generation&&r.common.lifecycle&&r.common.source_jar_hash==profile_.compatibility.jar&&r.profile==profile_.compatibility.profile,"Native FMC capture receipt incomplete");
 std::vector<std::uint8_t> bytes;bytes.reserve(r.common.total);
 for(std::uint32_t offset=0;offset<r.common.total;){auto q=request(w::Command::fetch,operation);q.offset=offset;q.total=r.common.total;auto part=exchange(q);
  require(part.common.phase==s::Phase::saved_verified&&part.common.total==r.common.total&&part.common.offset==offset&&part.common.artifact_hash==r.common.artifact_hash&&!part.common.data.empty()&&part.common.data.size()<=r.common.total-offset,"Native FMC capture no longer retained");
  bytes.insert(bytes.end(),part.common.data.begin(),part.common.data.end());offset+=static_cast<std::uint32_t>(part.common.data.size());
 }
 require(s::sha256(bytes)==r.common.artifact_hash,"Native FMC capture artifact identity");auto snapshot=inspect(bytes,profile_.records);require_compatible(snapshot,profile_.records,profile_.compatibility);
 const auto& p=snapshot.metadata.source;
 require(p.pid==target_.pid&&p.birth==target_.creation_filetime&&p.lifecycle==r.common.lifecycle&&p.generation==r.generation&&p.frequency==frequency_&&p.finish==r.common.observation_qpc&&p.finish<=n::qpc(),"Native FMC capture provenance");
 if(pending_.operation==operation){require(pending_.command==w::Command::capture&&p.lifecycle==pending_.lifecycle&&p.generation==pending_.generation&&p.observation>=pending_.issued,"Native FMC capture selection replaced");pending_={};}
 artifact=std::move(bytes);return r;
}
w::Result Client::recall(std::span<const std::uint8_t> artifact,std::uint64_t life,std::uint64_t generation,std::uint32_t timeout){
 require(timeout<=30000,"Native FMC wait bound");
 require(!pending_.operation,"Native FMC operation still unresolved");auto snapshot=inspect(artifact,profile_.records);require_compatible(snapshot,profile_.records,profile_.compatibility);
 auto state=status();if(!available(state,w::recall_memory))return unavailable(std::move(state));
 require(life&&life<=INT64_MAX&&generation&&generation<INT64_MAX&&state.common.lifecycle==life&&state.generation==generation,"Native FMC recall selection replaced");
 auto operation=++next_;last_=operation;pending_={w::Command::recall,operation,life,generation,n::qpc(),s::sha256(artifact)};
 for(std::size_t offset=0;offset<artifact.size();){auto q=request(w::Command::stage,operation,life,generation);q.total=static_cast<std::uint32_t>(artifact.size());q.offset=static_cast<std::uint32_t>(offset);auto count=std::min(w::chunk_bytes,artifact.size()-offset);q.data.assign(artifact.begin()+offset,artifact.begin()+offset+count);auto r=exchange(q);
  if(r.common.phase!=s::Phase::accepted){complete(r);return r;}
  require(r.common.total==q.total&&r.common.offset==offset+count,"Native FMC staging acknowledgement");offset+=count;
 }
 auto q=request(w::Command::recall,operation,life,generation);pending_.issued=q.issued_qpc;auto r=exchange(q);
 if(r.common.phase==s::Phase::accepted||r.common.phase==s::Phase::action_returned)r=wait(operation,timeout);else complete(r);return r;
}
}
