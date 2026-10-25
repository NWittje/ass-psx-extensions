#include <ios/cdu_blackout.hpp>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
namespace ios {
namespace {
namespace c=psx::cdu::blackout;
std::string normalized(std::string s) {
  std::transform(s.begin(),s.end(),s.begin(),[](unsigned char v){return char(std::tolower(v));});
  if(s=="localhost"||s=="::1")return "127.0.0.1";return s;
}
template<std::size_t N> std::string text(const std::array<char,N> &v) {return {v.data(),std::size_t(std::find(v.begin(),v.end(),'\0')-v.begin())};}
std::uint64_t creation(HANDLE p) {
  FILETIME birth{},exit{},kernel{},user{};
  if(!GetProcessTimes(p,&birth,&exit,&kernel,&user))return 0;
  return std::uint64_t(birth.dwLowDateTime)|(std::uint64_t(birth.dwHighDateTime)<<32);
}
bool same(const c::Packet &a,const c::Packet &b) {return a.epoch==b.epoch&&a.generation==b.generation&&a.revision==b.revision&&a.main==b.main;}
c::Packet echo(const c::Packet &s,c::Operation operation) {
  c::Packet p;p.operation=operation;p.epoch=s.epoch;p.generation=s.generation;p.revision=s.revision;p.main=s.main;return p;
}
}
std::string CduBlackoutResult::json() const {
  return "{\"state\":"+quote(state)+",\"reason\":"+quote(reason)+",\"mask\":"+std::to_string(mask)+",\"revision\":"+std::to_string(revision)+",\"render_observed\":false}";
}
CduBlackout::CduBlackout(std::wstring endpoint,DWORD pid):endpoint_(std::move(endpoint)),fixture_pid_(pid) {
  read_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);write_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
}
CduBlackout::~CduBlackout(){disconnected();if(read_.hEvent)CloseHandle(read_.hEvent);if(write_.hEvent)CloseHandle(write_.hEvent);}
void CduBlackout::close() {
  if(pipe_!=INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_,nullptr);DWORD n{};
    // Drain cancellation before member I/O storage can be reused or destroyed.
    if(reading_)GetOverlappedResult(pipe_,&read_,&n,TRUE);
    if(writing_)GetOverlappedResult(pipe_,&write_,&n,TRUE);
    CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;
  }
  if(process_){CloseHandle(process_);process_=nullptr;}
  reading_=writing_=awaiting_=bound_=false;status_.reset();pid_=0;creation_=0;
}
void CduBlackout::finish(std::string state,std::string reason) {
  if(action_)result_=CduBlackoutResult{action_,status_?status_->revision:0,status_?status_->active_mask:0,std::move(state),std::move(reason)};
  action_=0;expected_.reset();step_=Step::idle;mutated_=false;
}
void CduBlackout::lost(std::string why) {finish(mutated_?"indeterminate":"failed",why);close();reason_=std::move(why);}
void CduBlackout::disconnected(){lost("CDU control disconnected; no replay");}
bool CduBlackout::identity() const {
  ULONG pid{};return pipe_!=INVALID_HANDLE_VALUE&&process_&&GetNamedPipeServerProcessId(pipe_,&pid)&&pid==pid_&&
    WaitForSingleObject(process_,0)==WAIT_TIMEOUT&&creation(process_)==creation_;
}
bool CduBlackout::matches(const Packet &p) const {return p.main.port==port_&&normalized(text(p.main.host))==host_;}
bool CduBlackout::available() const {return pipe_!=INVALID_HANDLE_VALUE&&status_&&(status_->flags&c::ready_flag)&&matches(*status_);}
bool CduBlackout::connect() {
  if(!read_.hEvent||!write_.hEvent){reason_="CDU control I/O allocation unavailable";return false;}
  std::vector<std::pair<std::wstring,DWORD>> paths;
  if(!endpoint_.empty())paths.emplace_back(endpoint_,fixture_pid_);
  else {
    HANDLE list=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(list==INVALID_HANDLE_VALUE){reason_="CDU host discovery unavailable";return false;}
    PROCESSENTRY32W p{};p.dwSize=sizeof(p);
    if(Process32FirstW(list,&p))do{if(!_wcsicmp(p.szExeFile,L"psx-du-host.exe")&&paths.size()<8)paths.emplace_back(std::wstring(c::pipe_prefix)+std::to_wstring(p.th32ProcessID),p.th32ProcessID);}while(Process32NextW(list,&p));
    CloseHandle(list);
  }
  if(paths.empty()){reason_="CDU host is not running";return false;}
  for(std::size_t i=0;i<paths.size();++i) {
    const auto &[name,wanted]=paths[(candidate_+i)%paths.size()];
    HANDLE p=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(p==INVALID_HANDLE_VALUE){auto e=GetLastError();reason_=e==ERROR_PIPE_BUSY?"CDU control is owned by another IOS":e==ERROR_ACCESS_DENIED?"CDU control access unavailable":"CDU host control service unavailable";continue;}
    ULONG pid{};DWORD mode=PIPE_READMODE_MESSAGE;
    if(!GetNamedPipeServerProcessId(p,&pid)||(wanted&&pid!=wanted)||!SetNamedPipeHandleState(p,&mode,nullptr,nullptr)){CloseHandle(p);reason_="CDU host process association differs";continue;}
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    auto born=process?creation(process):0;
    if(!born){if(process)CloseHandle(process);CloseHandle(p);reason_="CDU host process identity unavailable";continue;}
    pipe_=p;process_=process;pid_=pid;creation_=born;++candidate_;reason_="Waiting for CDU host status";status_at_=0;return true;
  }return false;
}
std::uint64_t CduBlackout::request(unsigned selected,bool enabled,bool clear_all) {
  if(!available()||action_||!next_action_||(!clear_all&&(!selected||(selected&~c::all_units))))return 0;
  action_=next_action_++;selected_=selected;enabled_=enabled;clear_all_=clear_all;result_.reset();expected_=status_;
  step_=bound_?Step::mutation:Step::discovery;mutated_=false;return action_;
}
bool CduBlackout::receive(std::uint64_t id,CduBlackoutResult &out) {
  if(!result_||result_->request!=id)return false;out=*result_;result_.reset();return true;
}
void CduBlackout::transmit(Packet p,Purpose purpose,Tick t) {
  if(!identity()){lost("CDU host process changed; no replay");return;}
  if(!next_wire_){lost("CDU request IDs exhausted");return;}
  p.request=next_wire_++;if(!c::encode(p,outgoing_)){lost("Invalid CDU request encoding");return;}
  sent_=p;purpose_=purpose;awaiting_=true;deadline_=t+2000;
  ResetEvent(write_.hEvent);write_.Internal=write_.InternalHigh=0;write_.Offset=write_.OffsetHigh=0;
  DWORD n{};
  if(purpose==Purpose::mutation)mutated_=true;
  if(WriteFile(pipe_,outgoing_.data(),DWORD(outgoing_.size()),&n,&write_)){if(n!=outgoing_.size())lost("Partial CDU write; no replay");}
  else if(GetLastError()==ERROR_IO_PENDING)writing_=true;else lost("CDU write outcome unavailable; no replay");
}
void CduBlackout::accept(const Packet &p) {
  if(!identity()||p.request!=sent_.request||p.operation!=sent_.operation||p.selected_mask!=sent_.selected_mask||p.enabled!=sent_.enabled){lost("CDU reply association differs; no replay");return;}
  awaiting_=false;status_=p;reason_=text(p.message);
  if(!matches(p)){lost("CDU host is connected to a different PSX endpoint");return;}
  if(!(p.flags&c::ready_flag)){bound_=false;finish("failed","CDU Main source is not ready");reason_="CDU Main source is not ready";return;}
  if(purpose_==Purpose::background) {
    if(bound_&&(p.result!=c::Result::ok||!same(p,sent_)||!(p.flags&c::bound_flag))){bound_=false;finish("superseded","CDU control context changed; new action required");}
    return;
  }
  if(p.result!=c::Result::ok){bound_=false;finish(p.result==c::Result::stale?"superseded":"failed",text(p.message));return;}
  if(purpose_!=Purpose::mutation&&expected_&&!same(p,*expected_)){bound_=false;finish("superseded","CDU context changed before mutation");return;}
  if(purpose_==Purpose::discovery){step_=Step::bind;return;}
  if(purpose_==Purpose::bind){if(!(p.flags&c::bound_flag)){finish("failed","CDU binding unavailable");return;}bound_=true;step_=Step::mutation;return;}
  if(!expected_||p.epoch!=expected_->epoch||p.generation!=expected_->generation||p.main!=expected_->main||
    p.revision!=expected_->revision+1||!(p.flags&c::bound_flag)||
    p.active_mask!=(clear_all_?0u:enabled_?(expected_->active_mask|selected_):(expected_->active_mask&~selected_))) {
    lost("CDU mutation reply differs; no replay");return;
  }
  finish("accepted","CDU host accepted; client rendering is not observed");
}
void CduBlackout::io(Tick t) {
  DWORD n{};
  if(writing_) {
    if(GetOverlappedResult(pipe_,&write_,&n,FALSE)){writing_=false;if(n!=outgoing_.size()){lost("Partial CDU write; no replay");return;}}
    else if(GetLastError()!=ERROR_IO_INCOMPLETE){lost("CDU write outcome unavailable; no replay");return;}
  }
  if(reading_) {
    if(GetOverlappedResult(pipe_,&read_,&n,FALSE)) {
      reading_=false;Packet p;if(n!=incoming_.size()||!c::decode(incoming_,p)){lost("Malformed CDU reply; no replay");return;}accept(p);
    } else if(GetLastError()!=ERROR_IO_INCOMPLETE){lost("CDU readback unavailable; no replay");return;}
  }
  if(pipe_==INVALID_HANDLE_VALUE)return;
  if(awaiting_&&t>=deadline_){lost("CDU reply timed out; no replay");return;}
  if(awaiting_&&!reading_) {
    ResetEvent(read_.hEvent);read_.Internal=read_.InternalHigh=0;read_.Offset=read_.OffsetHigh=0;
    if(ReadFile(pipe_,incoming_.data(),DWORD(incoming_.size()),&n,&read_)) {
      Packet p;if(n!=incoming_.size()||!c::decode(incoming_,p)){lost("Malformed CDU reply; no replay");return;}accept(p);
    } else if(GetLastError()==ERROR_IO_PENDING)reading_=true;else lost("CDU readback unavailable; no replay");
  }
}
void CduBlackout::poll(Tick t,bool ready,std::string host,std::uint16_t port) {
  host=normalized(std::move(host));
  if(!ready){disconnected();return;}
  if(host_!=host||port_!=port){disconnected();host_=std::move(host);port_=port;connect_at_=0;}
  if(pipe_==INVALID_HANDLE_VALUE&&t>=connect_at_){connect_at_=t+1000;connect();}
  if(pipe_==INVALID_HANDLE_VALUE)return;
  io(t);if(pipe_==INVALID_HANDLE_VALUE||awaiting_||writing_)return;
  if(action_) {
    Packet p;
    if(step_==Step::discovery){p.operation=c::Operation::status;step_=Step::waiting;transmit(p,Purpose::discovery,t);}
    else if(step_==Step::bind){p=echo(*status_,c::Operation::bind);step_=Step::waiting;transmit(p,Purpose::bind,t);}
    else if(step_==Step::mutation){
      if(!bound_||!status_||!expected_||!same(*status_,*expected_)){finish("superseded","CDU binding changed before dispatch");return;}
      p=echo(*status_,clear_all_?c::Operation::clear_all:c::Operation::set);p.selected_mask=clear_all_?0:selected_;p.enabled=clear_all_?0:unsigned(enabled_);
      step_=Step::waiting;transmit(p,Purpose::mutation,t);
    }
  } else if(t>=status_at_) {
    Packet p;if(bound_&&status_)p=echo(*status_,c::Operation::status);
    status_at_=t+500;transmit(p,Purpose::background,t);
  }
}
std::string CduBlackout::json() const {
  return "{\"available\":"+std::string(available()?"true":"false")+",\"bound\":"+(bound_?"true":"false")+",\"reason\":"+quote(reason_)+
    ",\"now\":true,\"clear\":true,\"arming\":false,\"render_observed\":false,\"mask\":"+std::to_string(status_?status_->active_mask:0)+
    ",\"revision\":"+std::to_string(status_?status_->revision:0)+",\"host_pid\":"+std::to_string(pid_)+",\"host_creation_filetime\":"+std::to_string(creation_)+"}";
}
}
