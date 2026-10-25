#include <ios/weight_service.hpp>
#include <cmath>
namespace ios {
namespace wn=psx::instructor::weight_native;
namespace pi=psx::instructor;
namespace ipc=psx::non_normal;
namespace {
wn::Command action(std::string_view s){if(s=="align")return wn::Command::align;if(s=="cool")return wn::Command::cool;if(s=="quickstart")return wn::Command::quickstart;throw std::invalid_argument("Unknown service action");}
double converted(const Object&o){auto f=field<std::string>(o,"field");auto value=numeric(o,"value");auto units=field<std::string>(o,"units");if(units!="kg"&&units!="lb")throw std::invalid_argument("Invalid weight units");
 if(f=="zfw"||f=="fuel")return pi::native_weight(value,units=="lb");
 if(f=="gw_cg"){if(!std::isfinite(value)||std::abs(value*10-std::round(value*10))>1e-6)throw std::invalid_argument("CG uses 0.1 percent steps");return std::round(value*10);}return value;}
std::string real(double n){return std::to_string(n);}
std::string integer(const std::optional<int>&n,double scale=1){return n?real(*n*scale):"null";}
void observed(Operation&op,std::string detail,std::string readback){op.state="observed";op.detail=std::move(detail);op.result_json="{\"configured_readback\":"+quote(readback)+",\"physical_application_observed\":false}";}
}
void validate_weight_service(std::string_view kind,const Object&o){
 if(kind=="weight-action"){keys(o,{"request_id","expected_revision","action"});action(field<std::string>(o,"action"));}
 else {keys(o,{"request_id","expected_revision","field","value","units"});auto f=field<std::string>(o,"field");auto value=converted(o);if(f=="fuel"){if(value>400000)throw std::invalid_argument("Fuel outside PSX limits");}else pi::validate_weight(pi::weight_field(f),value);}
 field<std::string>(o,"request_id");field<std::uint64_t>(o,"expected_revision");
}
bool WeightService::available(unsigned mask)const {const auto freq=ipc::qpc_frequency();const auto time=ipc::qpc();return native_.connected()&&status_&&status_->phase==ipc::Phase::applied&&status_->reason==ipc::Reason::none&&(status_->capabilities&mask)&&status_->sampled_qpc>0&&time>=status_->sampled_qpc&&std::uint64_t(time-status_->sampled_qpc)<=freq*2;}
void WeightService::poll(Tick t,bool ready,ipc::ProcessIdentity id){
 if(!ready||!id.pid){if(native_.connected())native_.close();status_.reset();reply_.reset();identity_=id;return;}
 if(id!=identity_){native_.close();status_.reset();reply_.reset();identity_=id;poll_at_=0;}
 if(t>=poll_at_){poll_at_=t+250;if(!native_.connected()&&!native_.connect(id))return;native_.send(wn::Command::status);}
 wn::Result r;for(unsigned i=0;i<32&&native_.receive(r);++i){
  if(r.command==wn::Command::status)status_=r;
  if(dispatched_&&r.request==request_&&r.command==*dispatched_) {
   // A returned action belongs to its original lifecycle. Rejection/uncertainty
   // can legitimately describe a newer lifecycle after loading has superseded it.
   if(r.phase==ipc::Phase::action_returned&&r.lifecycle!=request_lifecycle_){r.phase=ipc::Phase::indeterminate;r.reason=ipc::Reason::replaced;}
   else if(r.phase==ipc::Phase::action_returned&&r.reason!=ipc::Reason::none)r.phase=ipc::Phase::indeterminate;
   reply_=r;
  }
 }
 if(!native_.connected())status_.reset();
}
void WeightService::disconnected(){settings_.disconnected();native_.close();status_.reset();reply_.reset();pending_.reset();dispatched_.reset();phase_=Phase::idle;identity_={};poll_at_=0;}
void WeightService::record(std::string_view line){settings_.record(line);if(line=="load1"||line=="exit"){disconnected();return;}if(phase_==Phase::read_fuel&&line.starts_with("Qs438="))fuel_seen_=true;}
void WeightService::flush(){if(!pending_)return;const auto c=*pending_;pending_.reset();dispatched_=c;request_lifecycle_=status_?status_->lifecycle:0;request_=native_.send(c,request_lifecycle_,c==wn::Command::fuel?fuel_lb_:0);send_failed_=request_==0;}
void WeightService::command(Operation&op,Tick t,bool first){
 try {
  if(first){reply_.reset();dispatched_.reset();send_failed_=false;fuel_seen_=false;request_=0;
   const auto&o=*op.command;
   if(op.kind=="weight-set"&&field<std::string>(o,"field")!="fuel") {settings_.set(pi::weight_field(field<std::string>(o,"field")),converted(o),t);phase_=Phase::public_set;}
   else {const auto c=op.kind=="weight-action"?action(field<std::string>(o,"action")):wn::Command::fuel;
    const auto bit=c==wn::Command::fuel?wn::fuel:c==wn::Command::align?wn::align:c==wn::Command::cool?wn::cool:wn::quickstart;
    if(!available(bit))throw std::runtime_error("PSX service unavailable");
    if(c==wn::Command::fuel){fuel_lb_=int(converted(o));if(fuel_lb_>status_->fuel_max_lb)throw std::invalid_argument("Fuel exceeds native capacity");}
    pending_=c;phase_=Phase::native;deadline_=t+5000;
   }
  }
  if(phase_==Phase::public_set){settings_.pump(t);if(!settings_.busy()){const auto&r=settings_.result();if(r.status==pi::SettingsStatus::match||r.status==pi::SettingsStatus::different)observed(op,r.detail,r.status==pi::SettingsStatus::match?"matching":"different");else {op.state="indeterminate";op.detail=r.detail;}phase_=Phase::idle;}}
  else if(phase_==Phase::native){
   if(reply_&&reply_->phase==ipc::Phase::action_returned){if(reply_->command==wn::Command::fuel){phase_=Phase::wait_fuel;deadline_=t+1000;}else {observed(op,reply_->command==wn::Command::quickstart?"Engine start requested":"Service action returned","not_applicable");op.result_json="{\"native_action_returned\":true,\"physical_application_observed\":false}";phase_=Phase::idle;}}
   else if(reply_&&(reply_->phase==ipc::Phase::rejected||reply_->phase==ipc::Phase::unavailable||reply_->phase==ipc::Phase::indeterminate)){op.state=reply_->phase==ipc::Phase::indeterminate?"indeterminate":"failed";op.detail=reply_->reason==ipc::Reason::replaced?"PSX loading or lifecycle changed":reply_->reason==ipc::Reason::expired?"Service request expired":reply_->reason==ipc::Reason::busy?"PSX service busy":"PSX service unavailable or action failed";phase_=Phase::idle;}
   else if(send_failed_||!native_.connected()||t>=deadline_){op.state="indeterminate";op.detail="Service result unavailable";phase_=Phase::idle;}
  }else if(phase_==Phase::wait_fuel&&t>=deadline_){phase_=Phase::read_fuel;deadline_=t+5000;if(!reader_.send("bang\n")){op.state="indeterminate";op.detail="Fuel readback unavailable";phase_=Phase::idle;}}
  else if(phase_==Phase::read_fuel){if(fuel_seen_&&settings_.state().fuel_lb){const bool match=std::abs(*settings_.state().fuel_lb-fuel_lb_)<=1;observed(op,match?"Fuel updated":"PSX returned a different fuel quantity",match?"matching":"different");op.result_json="{\"native_action_returned\":true,\"configured_readback\":"+quote(match?"matching":"different")+",\"fuel_lb\":"+real(*settings_.state().fuel_lb)+",\"physical_application_observed\":false}";phase_=Phase::idle;}else if(t>=deadline_){op.state="indeterminate";op.detail="Fuel readback unavailable";phase_=Phase::idle;}}
 }catch(const std::exception&e){op.state=first?"failed":"indeterminate";op.detail=e.what();phase_=Phase::idle;pending_.reset();settings_.cancel();}
}
std::string WeightService::snapshot()const {
 const auto&s=settings_.state();auto boolean=[](bool n){return n?"true":"false";};
 const auto bits=[&](const std::optional<int>&n,int mask){return n?std::string(boolean((*n&mask)==mask)):"null";};
 return "{\"zfw_lb\":"+integer(s.zfw_lb)+",\"gw_cg\":"+integer(s.gw_cg10,.1)+",\"fuel_lb\":"+(s.fuel_lb?real(*s.fuel_lb):"null")+
  ",\"ground_power\":"+bits(s.power_bits,9)+",\"ground_air\":"+bits(s.air_bits,1)+",\"engine_model\":"+integer(s.engine_model)+",\"fifth_engine\":"+bits(s.aero_bits,4)+
  ",\"zfw_min_lb\":"+std::to_string(pi::minimum_zfw(s))+",\"zfw_cg\":"+(available(wn::readout)?real(status_->zfw_cg):"null")+",\"fuel_max_lb\":"+(available(wn::limits|wn::fuel)?real(status_->fuel_max_lb):"null")+
  ",\"native\":{\"fuel\":"+boolean(available(wn::fuel))+",\"align\":"+boolean(available(wn::align))+",\"cool\":"+boolean(available(wn::cool))+",\"quickstart\":"+boolean(available(wn::quickstart))+",\"reason\":"+quote(available(wn::fuel|wn::align|wn::cool|wn::quickstart)?"":available(wn::readout|wn::limits)?"PSX service not supported by this exporter":"PSX service unavailable")+"}}";
}
}
