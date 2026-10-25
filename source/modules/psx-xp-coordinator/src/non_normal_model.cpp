#include "coordinator/non_normal.hpp"
#include <array>
#include <algorithm>

namespace coordinator {
namespace n=non_normal_wire;
namespace {
bool identity(n::ProcessIdentity p){return p.pid&&p.creation_filetime;}
bool terminal(n::Phase p){return p!=n::Phase::accepted&&p!=n::Phase::loading;}
}
struct NonNormalModel::Impl {
 struct Record {bool used{},internal{},queued{},sent{},uncertain{},complete{};std::uint64_t epoch{};n::Request input{},forward{};n::Result output{};};
 struct Response {std::uint64_t epoch{};n::Result value{};};
 std::array<Record,256> records{};std::size_t cursor{};
 std::array<Response,64> responses{};std::size_t response_head{},response_size{};
 NonNormalView view{};n::ProcessIdentity self{};std::uint64_t frequency{},ios_session{},plugin_session{},route_sequence{},ios_floor{},latest_mutation{};
 std::int64_t last_report{},heartbeat_due{};bool retire{};n::Request retirement{};
 void retirement_request(std::int64_t now){
  if(retire&&retirement.request)return; // Preserve the first owner until the IPC worker receives its Remove.
  retire=true;retirement={};if(!view.plugin_connected||!plugin_session||route_sequence==UINT64_MAX)return;
  retirement.wire_version=n::actor_version;retirement.command=n::Command::remove_actor;retirement.client=self;retirement.target=view.selected;retirement.session=plugin_session;retirement.request=++route_sequence;retirement.frequency=frequency;retirement.issued_qpc=now;retirement.deadline_qpc=now+static_cast<std::int64_t>(frequency);
  retirement.originator=ios_session?view.ios:self;retirement.originator_session=ios_session?ios_session:plugin_session;
 }
 n::Result response(const n::Request& r,n::Phase phase,n::Reason reason,std::int64_t now)const {
  n::Result value;value.wire_version=n::actor_version;value.server=self;value.session=r.session;value.request=r.request;value.command=r.command;value.phase=phase;value.reason=reason;value.observed_qpc=now;
  if(view.report_current){value.capabilities=view.report.capabilities;value.revision=view.report.revision;value.scenery_generation=view.report.scenery_generation;value.resource_generation=view.report.resource_generation;}
  return value;
 }
 bool emit(const n::Result& r){
  if(!view.ios_connected)return true;
  if(response_size==responses.size()){++view.faults;retire=true;return false;}
  responses[(response_head+response_size)%responses.size()]={view.ios_epoch,r};++response_size;return true;
 }
 Record* allocate(){for(std::size_t i=0;i<records.size();++i){const auto at=(cursor+i)%records.size();auto& r=records[at];if(!r.used||r.complete){r={};r.used=true;cursor=(at+1)%records.size();return &r;}}return nullptr;}
 void finish(Record& r,n::Phase phase,n::Reason reason,std::int64_t now){r.complete=true;r.queued=false;r.output=response(r.input,phase,reason,now);if(!r.internal)emit(r.output);}
 void clear(n::Reason reason,std::int64_t now){
  for(auto& r:records)if(r.used&&!r.complete){const bool handed_mutation=r.forward.request&&!r.queued&&(r.forward.command==n::Command::place_actor||r.forward.command==n::Command::remove_actor);finish(r,(r.sent||r.uncertain||handed_mutation)?n::Phase::indeterminate:n::Phase::rejected,reason,now);}
  view.plugin_connected=view.report_current=false;view.report={};last_report=heartbeat_due=0;plugin_session=0;
 }
 bool enqueue(Record& r,std::int64_t now){
  if(route_sequence==UINT64_MAX)return false;
  r.epoch=view.plugin_epoch;r.forward=r.input;r.forward.client=self;r.forward.target=view.selected;r.forward.session=plugin_session;r.forward.request=++route_sequence;
  r.forward.originator=r.internal?self:r.input.client;r.forward.originator_session=r.internal?plugin_session:r.input.session;
  r.forward.issued_qpc=r.internal?now:r.input.issued_qpc;
  // A forwarding hop may shorten but never renew the IOS operation deadline.
  r.forward.deadline_qpc=r.internal?now+static_cast<std::int64_t>(frequency):r.input.deadline_qpc;r.forward.frequency=frequency;
  r.queued=true;return n::valid(r.forward);
 }
 void pulse(std::int64_t now){
  if(!view.plugin_connected||retire||now<heartbeat_due)return;
  heartbeat_due=now+static_cast<std::int64_t>(frequency/4);
  auto* r=allocate();if(!r){++view.faults;retire=true;return;}
  r->internal=true;r->input.wire_version=n::actor_version;r->input.command=view.report_current?n::Command::heartbeat:n::Command::status;r->input.session=plugin_session;r->input.request=route_sequence+1;r->input.client=self;r->input.target=view.selected;
  if(!enqueue(*r,now)){r->complete=true;++view.faults;retire=true;}
 }
};
NonNormalModel::NonNormalModel():impl_(std::make_unique<Impl>()){}
NonNormalModel::~NonNormalModel()=default;
void NonNormalModel::configure(n::ProcessIdentity self,std::uint64_t frequency){*impl_={};impl_->self=self;impl_->frequency=frequency;}
void NonNormalModel::ios_open(std::uint64_t epoch,n::ProcessIdentity peer,std::int64_t now){
 auto& p=*impl_;if(p.view.ios_connected)ios_close(p.view.ios_epoch,now);p.view.ios_epoch=epoch;p.view.ios=peer;p.view.ios_connected=epoch&&identity(peer);p.ios_session=p.ios_floor=0;p.response_head=p.response_size=0;
}
void NonNormalModel::ios_close(std::uint64_t epoch,std::int64_t now){
 auto& p=*impl_;if(epoch!=p.view.ios_epoch){++p.view.obsolete;return;}p.retirement_request(now);p.view.ios_connected=false;p.response_head=p.response_size=0;p.clear(n::Reason::disconnected,now);p.ios_session=0;
}
void NonNormalModel::selected(n::ProcessIdentity selected,std::int64_t now){auto& p=*impl_;if(selected==p.view.selected)return;p.retirement_request(now);p.clear(n::Reason::replaced,now);p.view.selected=selected;}
void NonNormalModel::associate_psx(n::ProcessIdentity psx){impl_->view.psx=psx;}
void NonNormalModel::plugin_open(std::uint64_t epoch,n::ProcessIdentity peer,std::uint64_t session,std::int64_t now){
 auto& p=*impl_;if(!identity(peer)||peer!=p.view.selected||!epoch||!session){++p.view.faults;p.retire=true;return;}p.clear(n::Reason::replaced,now);p.view.plugin_epoch=epoch;p.view.plugin_connected=true;p.plugin_session=session;p.retire=false;p.pulse(now);
}
void NonNormalModel::plugin_close(std::uint64_t epoch,std::int64_t now){auto& p=*impl_;if(epoch!=p.view.plugin_epoch){++p.view.obsolete;return;}p.clear(n::Reason::transport,now);}
bool NonNormalModel::request(std::uint64_t epoch,const n::Request& input,std::int64_t now){
 auto& p=*impl_;if(!p.view.ios_connected||epoch!=p.view.ios_epoch){++p.view.obsolete;return true;}
 // Diagnose missing Main binding separately, while still validating all Place fields.
 auto shape=input;if(input.command==n::Command::place_actor&&!identity(input.psx))shape.psx=p.self;
 if(input.wire_version!=n::actor_version||!n::valid(shape)||input.client!=p.view.ios||input.target!=p.self||input.frequency!=p.frequency||input.issued_qpc>now||input.originator!=input.client||input.originator_session!=input.session||(p.ios_session&&p.ios_session!=input.session)){++p.view.faults;return false;}
 p.ios_session=input.session;
 for(auto& r:p.records)if(r.used&&!r.internal&&r.input.client==input.client&&r.input.session==input.session&&r.input.request==input.request){
  if(r.input!=input){++p.view.faults;return p.emit(p.response(input,n::Phase::rejected,n::Reason::identity,now));}++p.view.duplicates;return p.emit(r.output);
 }
 if(input.request<=p.ios_floor)return p.emit(p.response(input,n::Phase::rejected,n::Reason::identity,now));p.ios_floor=input.request;
 auto* r=p.allocate();if(!r)return p.emit(p.response(input,n::Phase::rejected,n::Reason::busy,now));r->input=input;++p.view.accepted;
 if(input.deadline_qpc<=now){p.finish(*r,n::Phase::rejected,n::Reason::expired,now);return true;}
 if(input.command==n::Command::reset_malfunctions){p.finish(*r,n::Phase::rejected,n::Reason::unsupported,now);return true;}
 if(input.command==n::Command::place_actor&&(!identity(p.view.psx)||!identity(input.psx)||input.psx!=p.view.psx)){p.finish(*r,n::Phase::rejected,identity(p.view.psx)&&identity(input.psx)?n::Reason::identity:n::Reason::binding,now);return true;}
 if(input.command==n::Command::heartbeat||(input.command==n::Command::status&&!p.view.plugin_connected)){
  r->complete=true;r->output=p.view.report_current?p.view.report:p.response(input,n::Phase::unavailable,n::Reason::binding,now);r->output.server=p.self;r->output.session=input.session;r->output.request=input.request;r->output.command=input.command;return p.emit(r->output);
 }
 if(!p.view.plugin_connected||(input.command!=n::Command::status&&!p.view.report_current)){p.finish(*r,n::Phase::unavailable,n::Reason::binding,now);return true;}
 if(input.command==n::Command::place_actor&&!(p.view.report.capabilities&n::actor_capability)){p.finish(*r,n::Phase::unavailable,p.view.report.reason==n::Reason::none?n::Reason::unsupported:p.view.report.reason,now);return true;}
 const auto pending=std::count_if(p.records.begin(),p.records.end(),[](const auto& item){return item.used&&!item.complete;});
 if(pending>32){p.finish(*r,n::Phase::rejected,n::Reason::busy,now);return true;}
 if(!p.enqueue(*r,now)){p.finish(*r,n::Phase::rejected,n::Reason::busy,now);return true;}
 r->output=p.response(input,n::Phase::accepted,n::Reason::none,now);if(input.command==n::Command::status)return true;p.latest_mutation=r->forward.request;return p.emit(r->output);
}
bool NonNormalModel::sent(std::uint64_t epoch,const n::Request& request,std::int64_t now){
 auto& p=*impl_;if(epoch!=p.view.plugin_epoch||!p.view.plugin_connected){++p.view.obsolete;return true;}
 for(auto& r:p.records)if(r.used&&r.epoch==epoch&&r.forward.request==request.request){if(r.complete){++p.view.obsolete;return true;}if(r.forward!=request||r.queued||r.sent||request.issued_qpc>now){++p.view.faults;return false;}r.sent=true;if(r.internal&&request.command==n::Command::heartbeat)r.complete=true;return true;}
 ++p.view.faults;return false;
}
bool NonNormalModel::uncertain_write(std::uint64_t epoch,const n::Request& request){
 auto& p=*impl_;if(epoch!=p.view.plugin_epoch||!p.view.plugin_connected){++p.view.obsolete;return true;}
 for(auto& r:p.records)if(r.used&&r.epoch==epoch&&r.forward.request==request.request){if(r.complete){++p.view.obsolete;return true;}if(r.forward!=request||r.queued||r.sent||r.uncertain){++p.view.faults;return false;}r.uncertain=true;return true;}
 ++p.view.faults;return false;
}
bool NonNormalModel::result(std::uint64_t epoch,const n::Result& result,std::int64_t now){
 auto& p=*impl_;if(epoch!=p.view.plugin_epoch||!p.view.plugin_connected){++p.view.obsolete;return true;}
 if(result.wire_version!=n::actor_version||!n::valid(result)||result.server!=p.view.selected||result.session!=p.plugin_session||result.observed_qpc>now||(result.capabilities&~n::actor_capability)||result.phase==n::Phase::action_returned){++p.view.faults;return false;}
 for(auto& r:p.records)if(r.used&&r.epoch==epoch&&r.forward.request==result.request){
  if(!r.sent||r.forward.command!=result.command||((result.command==n::Command::place_actor||result.command==n::Command::remove_actor)&&result.observed_qpc<r.forward.issued_qpc)){++p.view.faults;return false;}
  if(r.complete&&!(r.internal&&result.command==n::Command::heartbeat)){++p.view.obsolete;return true;}
  if(result.observed_qpc>=r.forward.deadline_qpc){p.finish(r,n::Phase::indeterminate,n::Reason::expired,now);p.retirement_request(now);p.clear(n::Reason::expired,now);return true;}
  if(p.view.report_current&&(result.scenery_generation<p.view.report.scenery_generation||result.revision<p.view.report.revision)){++p.view.obsolete;return true;}
  if(!r.internal&&result.command!=n::Command::status&&result.request<p.latest_mutation){p.finish(r,n::Phase::indeterminate,n::Reason::replaced,now);++p.view.obsolete;return true;}
  p.view.report=result;p.view.report_current=true;p.last_report=now;
  r.output=result;r.output.server=p.self;r.output.session=r.input.session;r.output.request=r.input.request;r.output.command=r.input.command;r.complete=terminal(result.phase);
  if(!r.internal)return p.emit(r.output);return true;
 }
 ++p.view.faults;return false;
}
void NonNormalModel::tick(std::int64_t now){
 auto& p=*impl_;
 bool expired=false;
 for(auto& r:p.records)if(r.used&&!r.complete&&r.forward.deadline_qpc&&now>=r.forward.deadline_qpc){p.finish(r,(r.sent||r.uncertain)?n::Phase::indeterminate:n::Phase::rejected,n::Reason::expired,now);expired=true;}
 if(expired){p.retirement_request(now);p.clear(n::Reason::expired,now);}
 p.pulse(now);
}
bool NonNormalModel::next_command(std::uint64_t& epoch,n::Request& request){auto& p=*impl_;Impl::Record* first{};for(auto& r:p.records)if(r.used&&r.queued&&!r.complete&&(!first||r.forward.request<first->forward.request))first=&r;if(!first)return false;first->queued=false;epoch=first->epoch;request=first->forward;return true;}
bool NonNormalModel::next_result(std::uint64_t& epoch,n::Result& result){auto& p=*impl_;if(!p.response_size)return false;const auto& r=p.responses[p.response_head];epoch=r.epoch;result=r.value;p.response_head=(p.response_head+1)%p.responses.size();--p.response_size;return true;}
bool NonNormalModel::take_retirement(n::Request& request)noexcept{const bool value=impl_->retire;request=impl_->retirement;impl_->retire=false;impl_->retirement={};return value;}
NonNormalView NonNormalModel::view()const{return impl_->view;}
}
