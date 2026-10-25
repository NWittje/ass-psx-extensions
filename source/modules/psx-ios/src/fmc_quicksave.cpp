#include <ios/fmc_quicksave.hpp>
#include <fstream>
#include <memory>

namespace ios {
namespace f=psx::fmc::native_records;
namespace w=psx::fmc::wire;
namespace s=psx::situation;
namespace n=psx::non_normal;
namespace {
std::vector<std::uint8_t> read_file(const std::filesystem::path& file) {
  std::ifstream in(file,std::ios::binary|std::ios::ate);
  if(!in)throw std::runtime_error("No FMC quicksave saved");
  const auto size=in.tellg();
  if(size<std::streamoff(f::header_bytes)||size>std::streamoff(s::max_artifact))
    throw std::runtime_error("FMC quicksave has invalid size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.seekg(0);in.read(reinterpret_cast<char*>(bytes.data()),size);
  if(!in||in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("FMC quicksave read failed");
  return bytes;
}
void validate(std::span<const std::uint8_t> bytes,const f::Profile& profile) {
  const auto snapshot=f::inspect(bytes,profile.records);
  f::require_compatible(snapshot,profile.records,profile.compatibility);
}
bool pending(s::Phase p){return p==s::Phase::accepted||p==s::Phase::action_returned;}
std::string failure(s::Reason reason){
  switch(reason){
  case s::Reason::unsupported:return "FMC service unavailable";
  case s::Reason::expired:return "FMC request expired";
  case s::Reason::replaced:return "PSX state changed before FMC action";
  case s::Reason::busy:return "FMC service busy";
  case s::Reason::native_exception:return "Native FMC action failed";
  default:return "FMC action rejected";
  }
}
}
FmcQuicksaveStore::FmcQuicksaveStore(std::filesystem::path directory)
  :file_(std::move(directory)/L"fmc-quicksave.psxfmc"),profile_(f::qualified_profile()){}
std::vector<std::uint8_t> FmcQuicksaveStore::read() const {
  auto bytes=read_file(file_);validate(bytes,profile_);return bytes;
}
void FmcQuicksaveStore::write(std::span<const std::uint8_t> bytes) {
  validate(bytes,profile_);
  auto temp=file_;temp+=L".new-"+std::to_wstring(n::nonce());
  auto backup=file_;backup+=L".bak";
  HANDLE out=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(out==INVALID_HANDLE_VALUE)throw std::runtime_error("FMC quicksave file could not be created");
  bool committed=false;
  try {
    std::size_t offset{};
    while(offset<bytes.size()){
      DWORD written{};
      if(!WriteFile(out,bytes.data()+offset,static_cast<DWORD>(bytes.size()-offset),&written,nullptr)||!written)
        throw std::runtime_error("FMC quicksave write failed");
      offset+=written;
    }
    if(!FlushFileBuffers(out))throw std::runtime_error("FMC quicksave flush failed");
    CloseHandle(out);out=INVALID_HANDLE_VALUE;
    if(read_file(temp)!=std::vector<std::uint8_t>(bytes.begin(),bytes.end()))throw std::runtime_error("FMC quicksave write differs");
    if(GetFileAttributesW(file_.c_str())!=INVALID_FILE_ATTRIBUTES){
      if(!ReplaceFileW(file_.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)){
        // ReplaceFile can move the old file before a later step fails. Recover
        // that exact backup only when the primary name is absent.
        if(GetFileAttributesW(file_.c_str())==INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES)
          MoveFileExW(backup.c_str(),file_.c_str(),MOVEFILE_WRITE_THROUGH);
        throw std::runtime_error("FMC quicksave replacement failed; previous save retained");
      }
    }else if(!MoveFileExW(temp.c_str(),file_.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("FMC quicksave commit failed");
    committed=true;
    if(read()!=std::vector<std::uint8_t>(bytes.begin(),bytes.end()))throw std::runtime_error("FMC quicksave committed readback differs; backup retained");
  }catch(...){
    if(out!=INVALID_HANDLE_VALUE)CloseHandle(out);
    if(!committed)DeleteFileW(temp.c_str()); // Only this invocation's owned staging file.
    throw;
  }
}
FmcQuicksave::FmcQuicksave(std::filesystem::path directory)
  :store_(std::move(directory)),worker_([this]{run();}){}
FmcQuicksave::~FmcQuicksave(){
  {std::lock_guard lock(mutex_);ending_=true;}cv_.notify_all();worker_.join();
}
void FmcQuicksave::context(bool ready,s::ProcessIdentity target){
  std::lock_guard lock(mutex_);
  if(ready!=main_ready_||target!=target_){main_ready_=ready;target_=target;cv_.notify_all();}
}
FmcQuicksave::State FmcQuicksave::state_locked() const {
  const bool fresh=checked_&&now()-checked_<3000;
  const bool available=main_ready_&&target_.pid&&fresh&&!busy_;
  State result;
  result.busy=busy_;result.saved_file=file_valid_;
  result.save_available=available&&(capabilities_&w::capture_memory);
  result.recall_available=available&&file_valid_&&(capabilities_&w::recall_memory);
  const auto reason=busy_?"FMC operation pending":!main_ready_?"PSX Main unavailable":!target_.pid?"Local PSX identity unavailable":!fresh?"FMC service unavailable":reason_;
  result.save_reason=result.save_available?"":reason;
  result.recall_reason=result.recall_available?"":(!file_valid_?file_reason_:reason);
  return result;
}
FmcQuicksave::State FmcQuicksave::state() const {std::lock_guard lock(mutex_);return state_locked();}
bool FmcQuicksave::busy() const {std::lock_guard lock(mutex_);return busy_;}
std::string FmcQuicksave::json() const {
  const auto a=state();return "{\"save_available\":"+std::string(a.save_available?"true":"false")+",\"recall_available\":"+(a.recall_available?"true":"false")+",\"saved_file\":"+(a.saved_file?"true":"false")+",\"busy\":"+(a.busy?"true":"false")+",\"save_reason\":"+quote(a.save_reason)+",\"recall_reason\":"+quote(a.recall_reason)+"}";
}
void FmcQuicksave::command(Operation& op,bool first){
  std::lock_guard lock(mutex_);
  if(first){
    const auto status=state_locked();const bool recall=op.kind=="fmc-recall";
    if(!(recall?status.recall_available:status.save_available)){op.state="failed";op.detail=recall?status.recall_reason:status.save_reason;return;}
    busy_=true;job_=Job{op.id,recall,target_};outcome_=Outcome{op.id,"sent",recall?"Recalling FMC":"Saving FMC"};cv_.notify_all();
  }
  if(outcome_&&outcome_->id==op.id){op.state=outcome_->state;op.detail=outcome_->detail;op.result_json=outcome_->result;}
}
void FmcQuicksave::apply(Engine& engine) const {
  std::lock_guard lock(mutex_);
  if(outcome_)if(auto op=engine.find(outcome_->id)){op->state=outcome_->state;op->detail=outcome_->detail;op->result_json=outcome_->result;if(!busy_)engine.native_pause_finished(*op);}
}
void FmcQuicksave::run(){
  const auto profile=f::qualified_profile();
  std::unique_ptr<f::Client> client;bool connected=false;
  s::ProcessIdentity selected{};std::optional<Job> active;std::uint64_t native{};
  bool committing=false;Tick status_at{};
  auto file_state=[&](){try{store_.read();std::lock_guard lock(mutex_);file_valid_=true;file_reason_.clear();}catch(const std::exception& e){std::lock_guard lock(mutex_);file_valid_=false;file_reason_=e.what();}};
  auto publish=[&](std::string state,std::string detail,bool finished,const w::Result* r=nullptr){
    std::lock_guard lock(mutex_);
    std::string result="{\"native_operation\":"+std::to_string(native);
    if(r)result+=",\"native_phase\":"+std::to_string(static_cast<unsigned>(r->common.phase));
    result+="}";outcome_=Outcome{active->id,std::move(state),std::move(detail),std::move(result)};
    if(finished){busy_=false;active.reset();native=0;committing=false;}
  };
  file_state();
  for(;;){
    bool ready{};s::ProcessIdentity target;
    {std::lock_guard lock(mutex_);if(ending_)break;ready=main_ready_;target=target_;if(job_){active=job_;job_.reset();native=0;committing=false;}}
    if(active&&target.pid&&target!=active->target){publish("indeterminate","PSX session changed; FMC result unavailable",true);client.reset();connected=false;}
    if(!active&&(!ready||!target.pid)){
      if(client)client->disconnect();connected=false;
      std::lock_guard lock(mutex_);capabilities_=0;checked_=0;
    }else if(!active&&now()<status_at){}else try{
      const auto required=active?active->target:target;
      if(!client||required!=selected){client=std::make_unique<f::Client>(required);selected=required;connected=false;}
      if(!connected){client->connect();connected=true;status_at=0;}
      if(active){
        w::Result r;std::vector<std::uint8_t> bytes;
        if(!native){
          // Recheck the current Main selection immediately before submission;
          // service preflight independently checks its lifecycle/generation.
          bool current;{std::lock_guard lock(mutex_);current=main_ready_&&target_==active->target;}
          if(!current){publish("superseded","PSX changed before FMC action",true);continue;}
          const auto previous=client->last_operation();
          try{
            if(active->recall){bytes=store_.read();auto state=client->status();r=client->recall(bytes,state.common.lifecycle,state.generation,0);}
            else r=client->capture(bytes,0);
          }catch(...){if(client->last_operation()!=previous)native=client->last_operation();throw;}
          if(client->last_operation()!=previous)native=client->last_operation();
        }else r=client->lookup(native);
        if(pending(r.common.phase)){publish("sent",active->recall?"Recalling FMC":"Saving FMC",false,&r);}
        else if(r.common.reason==s::Reason::not_retained||r.common.phase==s::Phase::unavailable){publish(native?"indeterminate":"failed",native?"FMC result unavailable":failure(r.common.reason),!native,&r);}
        else if(!active->recall&&r.common.phase==s::Phase::saved_verified){
          if(bytes.empty())client->fetch_capture(native,bytes);
          committing=true;store_.write(bytes);file_state();publish("observed","FMC saved",true,&r);
        }else if(active->recall&&r.common.phase==s::Phase::observed){publish("observed","FMC recalled \xE2\x80\x94 paused",true,&r);}
        else publish(r.common.phase==s::Phase::indeterminate?"indeterminate":"failed",failure(r.common.reason),true,&r);
      }else if(now()>=status_at){
        const auto r=client->status();status_at=now()+1000;
        const bool qualified=client->native_peer()&&r.common.phase==s::Phase::observed&&(r.common.capabilities&w::native_records)&&(r.validity&w::compatibility)&&r.common.source_jar_hash==profile.compatibility.jar&&r.profile==profile.compatibility.profile&&r.common.lifecycle&&r.generation;
        {std::lock_guard lock(mutex_);checked_=now();capabilities_=qualified?r.common.capabilities:0;reason_="FMC service unavailable";}
        // FMC and situation calls share one exporter pipe. Release idle status
        // ownership; an executing operation retains its original connection.
        client->disconnect();connected=false;
      }
    }catch(const std::exception& e){
      if(client)client->disconnect();connected=false;status_at=now()+1000;
      {std::lock_guard lock(mutex_);checked_=0;capabilities_=0;reason_="FMC service unavailable";}
      if(active){
        if(committing){file_state();publish("failed",std::string("FMC captured; ")+e.what(),true);}
        else if(native)publish("indeterminate","FMC result unavailable",false);
        else {file_state();publish("failed",e.what(),true);}
      }
    }
    std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(1000));
  }
}
}
