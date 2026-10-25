#include "hwio/thrust_control.hpp"
#include <algorithm>
#include <string_view>
namespace hwio {namespace {
namespace t=psx::hwio::thrust_control;
template<std::size_t N>void text(std::array<char,N>& out,std::string_view value){out={};std::copy_n(value.begin(),std::min(N-1,value.size()),out.begin());}
bool same(const auto& a,const t::Packet& b){return a.operation==b.operation && a.session==b.session && a.revision==b.revision && a.token==b.token && (a.operation!=t::Operation::set_hold || a.percent==b.percent);}
std::uint32_t group_bit(unsigned group){return group>=1 && group<=20?1u<<(group-1):group>=101 && group<=102?1u<<(group-81):0;}
bool alive(t::Process peer){
    const auto p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,peer.pid);if(!p)return GetLastError()!=ERROR_INVALID_PARAMETER;
    FILETIME birth{},exit{},kernel{},user{};const bool known=GetProcessTimes(p,&birth,&exit,&kernel,&user)!=FALSE;
    const bool result=WaitForSingleObject(p,0)!=WAIT_OBJECT_0 && (!known || ((std::uint64_t(birth.dwHighDateTime)<<32)|birth.dwLowDateTime)==peer.birth);CloseHandle(p);return result;
}
}
ThrustFrame thrust_frame(std::array<t::Lever,4> mapped,const std::array<t::Lever,4>& fdstq,std::uint64_t revision,std::uint32_t released){
    ThrustFrame frame;frame.revision=revision+1;frame.released_groups=released;
    auto mix=[&](std::uint64_t x){frame.revision=(frame.revision^x)*1099511628211ull;};
    for(unsigned i=0;i<4;++i){auto& l=mapped[i];if(fdstq[i].flags&t::covered){if(l.flags&t::covered){l.flags&=~t::ready;text(l.reason,"Conflicting motor and FDSTQ throttle assignments");}else l=fdstq[i];}
        mix(l.group);mix(l.master);mix(l.generation);mix(l.calibration);mix(l.flags&t::covered);frame.levers[i]=l;}
    if(!frame.revision)frame.revision=1;return frame;
}
ThrustControl::ThrustControl(std::uint64_t session){status_.session=session;next_token_=session?session:1;}
ThrustControl::~ThrustControl(){if(process_)CloseHandle(process_);}
void ThrustControl::relinquish(){if(process_)CloseHandle(process_);process_=nullptr;owner_={};status_.owner_pid=0;status_.token=0;releasing_=interrupted_=false;cause_={};}
void ThrustControl::begin_release(){if(!releasing_)release_after_=observations_;releasing_=true;interrupted_=false;cause_={};}
void ThrustControl::flags(){
    bool covered=true,ready=true,held=true;
    for(const auto& l:status_.levers){covered&=(l.flags&t::covered)!=0;ready&=(l.flags&t::ready)!=0;held&=l.phase==t::Phase::held && l.operation_id==status_.operation_id;}
    if(owns() && releasing_ && observations_>release_after_ && (released_groups_&claimed_groups_)==claimed_groups_)relinquish();
    status_.flags=(covered?t::all_covered:0)|(covered && ready && !releasing_?t::can_set:0)|(owns()?t::owned:0)|(drives() && held?t::all_held:0)|(releasing_?t::releasing:0)|(interrupted_?t::interrupted:0);
}
void ThrustControl::observe(const ThrustFrame& frame,std::int64_t qpc,std::uint64_t frequency){
    ++observations_;released_groups_=frame.released_groups;status_.revision=frame.revision;status_.sampled_qpc=qpc;status_.frequency=frequency;status_.levers=frame.levers;
    if(owns() && process_ && WaitForSingleObject(process_,0)==WAIT_OBJECT_0)begin_release();
    if(owns() && !releasing_){
        for(const auto& l:frame.levers)if(!(l.flags&t::ready)){interrupted_=true;if(!cause_[0])text(cause_,std::string_view(l.reason.data(),strnlen_s(l.reason.data(),l.reason.size())));}
        if(frame.revision!=claimed_revision_){interrupted_=true;if(!cause_[0])text(cause_,"Throttle assignment or hardware continuity changed; release IOS ownership");}
    }
    if(owns())for(unsigned i=0;i<4;++i){auto& l=status_.levers[i];l.requested_percent=requested_[i];l.effective_percent=requested_[std::min(l.master,3u)];if(releasing_)l.phase=t::Phase::releasing;else if(interrupted_){l.phase=t::Phase::interrupted;if(cause_[0] && !l.reason[0])text(l.reason,std::string_view(cause_.data()));}}
    flags();
}
std::array<double,4> ThrustControl::targets()const noexcept {std::array<double,4> result{};for(unsigned i=0;i<4;++i)result[i]=requested_[std::min(status_.levers[i].master,3u)];return result;}
ThrustControl::Packet ThrustControl::dispatch(const Packet& request,Process peer){
    Packet reply;reply.operation=request.operation;reply.request_id=request.request_id;reply.session=status_.session;reply.revision=status_.revision;reply.token=status_.token;reply.operation_id=status_.operation_id;
    auto finish=[&]{flags();reply.status=snapshot(peer);return reply;};
    if(!t::valid(request)){reply.result=t::Result::malformed;return finish();}
    if(request.operation==t::Operation::status)return finish();
    if(!peer.pid || !peer.birth){reply.result=t::Result::unavailable;text(reply.message,"Pipe client identity unavailable");return finish();}
    for(const auto& receipt:receipts_)if(receipt.request.request_id==request.request_id && receipt.request.session==request.session && receipt.peer==peer){
        if(!same(receipt.request,request)){reply.result=t::Result::conflict;text(reply.message,"Request ID reused with different command");return finish();}
        reply.result=receipt.result;reply.token=receipt.token;reply.operation_id=receipt.operation;reply.message=receipt.message;return finish();
    }
    if(request.session!=status_.session){reply.result=t::Result::session_mismatch;return finish();}
    if(owns() && peer!=owner_){reply.result=t::Result::busy;text(reply.message,"Another IOS process owns thrust positioning");return finish();}
    auto client=std::find_if(clients_.begin(),clients_.end(),[&](const auto& c){return c.peer==peer;});
    if(client!=clients_.end() && request.request_id<=client->highwater){reply.result=t::Result::conflict;text(reply.message,"Stale thrust request ID");return finish();}
    if(client==clients_.end())client=std::find_if(clients_.begin(),clients_.end(),[](const auto& c){return !c.peer.pid;});
    if(client==clients_.end())client=std::find_if(clients_.begin(),clients_.end(),[](const auto& c){return !alive(c.peer);});
    if(client==clients_.end()){reply.result=t::Result::busy;text(reply.message,"Thrust client ledger full");return finish();}
    if(request.operation==t::Operation::release){
        if(!owns() || !request.token || request.token!=status_.token){reply.result=t::Result::conflict;text(reply.message,"Thrust ownership token changed");}
        else {begin_release();status_.operation_id=request.request_id;reply.operation_id=request.request_id;text(reply.message,"Thrust release requested; awaiting drive release");}
    }else if(request.revision!=status_.revision || (owns()?request.token!=status_.token:request.token!=0)){reply.result=t::Result::conflict;text(reply.message,"Thrust session, assignment or ownership changed");}
    else if(!(status_.flags&t::can_set)){reply.result=t::Result::unavailable;text(reply.message,"All four thrust-lever drives must be calibrated and ready");}
    else {
        HANDLE process=process_;if(!process){process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,peer.pid);FILETIME birth{},exit{},kernel{},user{};if(!process || !GetProcessTimes(process,&birth,&exit,&kernel,&user) || ((std::uint64_t(birth.dwHighDateTime)<<32)|birth.dwLowDateTime)!=peer.birth || WaitForSingleObject(process,0)!=WAIT_TIMEOUT){if(process)CloseHandle(process);reply.result=t::Result::unavailable;text(reply.message,"IOS process identity changed");return finish();}}
        process_=process;owner_=peer;status_.owner_pid=peer.pid;if(!owns()){if(++next_token_==0)++next_token_;status_.token=next_token_;}claimed_revision_=status_.revision;requested_=request.percent;status_.operation_id=request.request_id;releasing_=interrupted_=false;cause_={};claimed_groups_=0;for(const auto& l:status_.levers)claimed_groups_|=group_bit(l.group);
        for(unsigned i=0;i<4;++i){auto& l=status_.levers[i];l.phase=t::Phase::positioning;l.operation_id=0;l.requested_percent=requested_[i];l.effective_percent=requested_[l.master];}
        reply.token=status_.token;reply.operation_id=status_.operation_id;text(reply.message,"Thrust positioning admitted; physical arrival pending");
    }
    *client={peer,request.request_id};
    receipts_[receipt_cursor_++%receipts_.size()]={Request(request),peer,reply.result,reply.token,reply.operation_id,reply.message};
    return finish();
}
}
