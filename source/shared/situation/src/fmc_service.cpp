#include <psx/situation/fmc_service.hpp>
#include <algorithm>
#include <stdexcept>

namespace psx::fmc {namespace n=psx::non_normal;namespace s=psx::situation;namespace w=wire;
namespace {
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
w::Result unavailable(w::Result result){result.common.phase=s::Phase::unavailable;if(result.common.reason==s::Reason::none)result.common.reason=static_cast<s::Reason>(w::Reason::profile);result.common.evidence=0;result.validity=0;return result;}
Compatibility compatibility(const w::Result& state){
 require((state.validity&w::Validity::compatibility)!=0&&(state.model==1||state.model==2),"FMC current compatibility unavailable");
 return {state.common.source_jar_hash,state.profile,state.navigation_database,state.model_configuration,static_cast<Model>(state.model)};
}
}
Hash memory_hash(const Snapshot& snapshot){
 // Hash only the canonical owned graph. Source time/generation and diagnostic
 // aircraft context naturally differ in a post-recall observation.
 auto bytes=encode(snapshot);const auto graph=header_bytes+2+snapshot.source_version.size()+32+48;
 return s::sha256(std::span<const std::uint8_t>(bytes).subspan(graph));
}
Client::Client(s::ProcessIdentity target):target_(target),self_(n::current_process()),session_(n::nonce()),frequency_(n::qpc_frequency()){}
Client::~Client(){disconnect();}
void Client::disconnect()noexcept{if(pipe_!=INVALID_HANDLE_VALUE){CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}sent_=received_=0;}
void Client::open(){
 disconnect();pipe_=CreateFileW(s::endpoint(target_).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);s::ProcessIdentity actual{};
 if(pipe_==INVALID_HANDLE_VALUE||!n::authenticate(pipe_,false,target_,actual)){disconnect();throw std::runtime_error("FMC service unavailable or process identity changed");}
}
w::Request Client::request(w::Command command,std::uint64_t operation,std::uint64_t life,std::uint64_t generation){
 w::Request r;r.command=command;r.client=self_;r.target=target_;r.session=session_;r.operation=operation;r.lifecycle=life;r.generation=generation;r.frequency=frequency_;r.issued_qpc=n::qpc();r.deadline_qpc=r.issued_qpc+static_cast<std::int64_t>(frequency_*30);return r;
}
w::Result Client::exchange(const w::Request& r){
 require(pipe_!=INVALID_HANDLE_VALUE,"FMC service disconnected");s::Frame frame;
 if(sent_==UINT32_MAX||!w::encode(r,++sent_,frame)||!s::write_frame(pipe_,frame)||!s::read_frame(pipe_,frame,1500)){
  disconnect();throw std::runtime_error("FMC transport outcome uncertain; reconnect and look up, never replay a mutation");
 }
 w::Result result;std::uint32_t sequence{};
 if(!w::decode({frame.bytes.data(),frame.size},result,sequence)||sequence<=received_||result.common.server!=target_||result.common.session!=r.session||result.common.operation!=r.operation){disconnect();throw std::runtime_error("FMC response identity or revision");}
 received_=sequence;return result;
}
void Client::connect(){
 legacy_=false;open();
 try{status();return;}catch(...){disconnect();}
 // A read-only revision probe may reconnect to v1. No accepted mutation is
 // repeated, and v1 export is never advertised as complete custom memory.
 open();legacy_=true;try{status();}catch(...){disconnect();throw;}
}
w::Result Client::status(){
 if(!legacy_)return exchange(request(w::Command::status,++next_));
 require(pipe_!=INVALID_HANDLE_VALUE,"FMC historical service disconnected");
 auto q=request(w::Command::status,++next_);s::Request old;old.client=q.client;old.target=q.target;old.session=q.session;old.operation=q.operation;old.frequency=q.frequency;old.issued_qpc=q.issued_qpc;old.deadline_qpc=q.deadline_qpc;
 s::Frame frame;s::Result result;std::uint32_t sequence{};
 if(sent_==UINT32_MAX||!s::encode(old,++sent_,frame)||!s::write_frame(pipe_,frame)||!s::read_frame(pipe_,frame,1500)||!s::decode({frame.bytes.data(),frame.size},result,sequence)||sequence<=received_||result.server!=target_||result.session!=session_||result.operation!=q.operation){disconnect();throw std::runtime_error("FMC historical capability probe failed");}
 received_=sequence;w::Result response;response.common=std::move(result);response.common.reason=s::Reason::unsupported;return unavailable(std::move(response));
}
w::Result Client::lookup(std::uint64_t operation){if(legacy_){auto result=unavailable(status());result.common.operation=operation;return result;}return exchange(request(w::Command::lookup,operation));}
w::Result Client::wait(std::uint64_t operation,std::uint32_t timeout){auto end=GetTickCount64()+timeout;w::Result result;do{result=lookup(operation);if(result.common.phase!=s::Phase::accepted)return result;Sleep(20);}while(GetTickCount64()<end);return result;}
w::Result Client::capture(std::vector<std::uint8_t>& artifact){
 artifact.clear();auto state=status();if(legacy_||!(state.common.capabilities&w::capture_memory))return unavailable(std::move(state));
 auto expected=compatibility(state);auto operation=++next_;last_operation_=operation;auto r=exchange(request(w::Command::capture,operation,state.common.lifecycle,state.generation));
 if(r.common.phase==s::Phase::accepted)r=wait(operation,30000);
 if(r.common.phase!=s::Phase::saved_verified)return r;
 require((r.common.evidence&w::complete_checkpoint)&&r.generation,"FMC capture lacks complete source evidence");artifact.reserve(r.common.total);
 for(std::uint32_t offset=0;offset<r.common.total;){auto q=request(w::Command::fetch,operation);q.offset=offset;q.total=r.common.total;auto part=exchange(q);
  require(part.common.phase==s::Phase::saved_verified&&part.common.artifact_hash==r.common.artifact_hash&&part.common.total==r.common.total&&part.common.offset==offset&&!part.common.data.empty()&&part.common.data.size()<=r.common.total-offset,"FMC capture no longer retained");
  artifact.insert(artifact.end(),part.common.data.begin(),part.common.data.end());offset+=static_cast<std::uint32_t>(part.common.data.size());
 }
 require(artifact.size()==r.common.total&&s::sha256(artifact)==r.common.artifact_hash,"FMC capture artifact identity");auto snapshot=inspect(artifact);require_compatible(snapshot,expected);
 require(snapshot.source_pid==target_.pid&&snapshot.process_birth==target_.creation_filetime&&snapshot.lifecycle==r.common.lifecycle&&snapshot.generation==r.generation&&snapshot.frequency==frequency_,"FMC capture native provenance");return r;
}
w::Result Client::recall(std::span<const std::uint8_t> artifact,std::uint64_t life,std::uint64_t generation){
 auto snapshot=inspect(artifact);auto state=status();if(legacy_||!(state.common.capabilities&w::recall_memory))return unavailable(std::move(state));
 require_compatible(snapshot,compatibility(state));require(life&&generation&&state.common.lifecycle==life&&state.generation==generation,"FMC recall selected lifecycle/generation replaced");
 auto operation=++next_;last_operation_=operation;
 for(std::size_t offset=0;offset<artifact.size();){auto q=request(w::Command::stage,operation,life,generation);q.offset=static_cast<std::uint32_t>(offset);q.total=static_cast<std::uint32_t>(artifact.size());auto size=std::min(w::chunk_bytes,artifact.size()-offset);q.data.assign(artifact.begin()+offset,artifact.begin()+offset+size);auto accepted=exchange(q);if(accepted.common.phase!=s::Phase::accepted)return accepted;offset+=size;}
 auto result=exchange(request(w::Command::recall,operation,life,generation));if(result.common.phase==s::Phase::accepted)result=wait(operation,30000);
 if(result.common.phase==s::Phase::observed){
  require(result.common.artifact_hash==s::sha256(artifact)&&result.common.lifecycle==life&&generation!=UINT64_MAX&&result.generation==generation+1,"FMC recall completion identity");
  require((result.validity&(w::memory_readback|w::preservation))==(w::memory_readback|w::preservation)&&(result.common.evidence&(w::restored_readback|w::unrelated_preserved|s::pause_observed))==(w::restored_readback|w::unrelated_preserved|s::pause_observed)&&result.restored_state==memory_hash(snapshot)&&result.common.readback[9]==1&&result.common.readback[10]==(result.entry_pause_mask|1u)&&result.common.readback[11]==0,"FMC recall lacks native readback/preservation/Pause evidence");
 }
 return result;
}
}
