#include <ios/failure_batch.hpp>
#include <ios/airport.hpp>
#include <algorithm>
#include <set>
namespace ios {
namespace {
using namespace psx::instructor;
namespace h=psx::hwio::failures;
std::string native_value(const Object &o) {if(auto it=o.find("value");it!=o.end()&&std::holds_alternative<std::string>(it->second))return field<std::string>(o,"value");const auto n=numeric(o,"value");if(std::trunc(n)!=n||std::abs(n)>10000000)throw std::invalid_argument("Failure value must be an integer");return std::to_string(int(n));}
std::string status(SettingsStatus s) {return s==SettingsStatus::match?"matching":s==SettingsStatus::activated?"activated":s==SettingsStatus::different?"different":s==SettingsStatus::pending?"pending":"unavailable";}
}
std::vector<FailureBatchEntry> failure_batch_entries(const Object &o) {
  keys(o,{"request_id","expected_revision","action","entries"});
  field<std::string>(o,"request_id");field<std::uint64_t>(o,"expected_revision");
  const auto action=field<std::string>(o,"action");if(action!="set"&&action!="clear")throw std::invalid_argument("Invalid failure batch action");
  const bool clear=action=="clear";const auto &items=field<Array>(o,"entries");
  if(items.empty()||items.size()>32)throw std::invalid_argument("Select 1 to 32 failures");
  std::vector<FailureBatchEntry> result;std::set<unsigned> native_ids;unsigned hwio_effects{},cdu_effects{};
  for(const auto &item:items) {
    auto entry=object(item);const auto source=field<std::string>(entry,"source");FailureBatchEntry parsed;
    if(source=="psx") {
      if(clear)keys(entry,{"source","failure_id"});else keys(entry,{"source","failure_id","severe","criterion","value"});
      const auto id=field<std::uint64_t>(entry,"failure_id");if(id>UINT_MAX)throw std::invalid_argument("Unknown failure");failure_info(unsigned(id));
      if(!native_ids.insert(unsigned(id)).second)throw std::invalid_argument("Duplicate native failure");
      FailureEdit edit;edit.id=unsigned(id);
      if(!clear) {
        edit.severe=field<bool>(entry,"severe");const auto criterion=field<std::string>(entry,"criterion");
        if(criterion=="minutes") {edit.criterion=FailureCriterion::utc_time;const auto minutes=numeric(entry,"value");if(minutes<1||minutes>1439||std::trunc(minutes)!=minutes)throw std::invalid_argument("Simulated delay must be 1 to 1439 minutes");edit.delay_minutes=unsigned(minutes);}
        else {edit.criterion=failure_criterion(criterion);edit.value=native_value(entry);}
        validate_failure_edit(edit);
      }
      parsed.native=std::move(edit);
    } else if(source=="hwio") {
      if(clear)keys(entry,{"source","control"});else keys(entry,{"source","control","criterion","value"});
      const auto control=field<std::string>(entry,"control");
      parsed.axis_mask=control=="elevator"?h::elevator:control=="aileron"?h::aileron:control=="rudder"?h::rudder:0;parsed.trim=control=="trim";
      if(!parsed.axis_mask&&!parsed.trim)throw std::invalid_argument("Unknown HWIO control");
      const unsigned effect=parsed.axis_mask|(parsed.trim?8u:0u);if(hwio_effects&effect)throw std::invalid_argument("Duplicate HWIO control");hwio_effects|=effect;
      if(!clear) {
        parsed.criterion=field<std::string>(entry,"criterion");
        if(parsed.criterion=="immediate") {if(native_value(entry)!="")throw std::invalid_argument("Now does not take a threshold");}
        else {
          parsed.threshold=numeric(entry,"value");
          if(parsed.criterion!="ias"&&parsed.criterion!="altitude")throw std::invalid_argument("HWIO supports Now, IAS or native altitude only");
          if(std::trunc(parsed.threshold)!=parsed.threshold||parsed.threshold<(parsed.criterion=="ias"?0:-90)||parsed.threshold>(parsed.criterion=="ias"?600:50000))throw std::invalid_argument("HWIO trigger outside supported range");
        }
      }
    } else if(source=="cdu") {
      if(clear)keys(entry,{"source","unit"});else keys(entry,{"source","unit","criterion","value"});
      const auto unit=field<std::string>(entry,"unit");parsed.cdu_mask=unit=="L"?1u:unit=="C"?4u:unit=="R"?2u:0u;
      if(!parsed.cdu_mask)throw std::invalid_argument("Unknown CDU unit");
      if(cdu_effects&parsed.cdu_mask)throw std::invalid_argument("Duplicate CDU unit");cdu_effects|=parsed.cdu_mask;
      if(!clear&&(field<std::string>(entry,"criterion")!="immediate"||native_value(entry)!=""))throw std::invalid_argument("CDU blackout supports Now and Clear only");
    } else throw std::invalid_argument("Unknown failure source");
    result.push_back(std::move(parsed));
  }
  return result;
}
void FailureBatch::start(const Object &o,std::uint64_t operation,Tick t) {
  entries_=failure_batch_entries(o);clearing_=field<std::string>(o,"action")=="clear";
  hardware_at_=0;request_=cdu_request_=0;hardware_failed_=false;hardware_results_.clear();hardware_detail_.clear();hardware_source_="hwio";hardware_state_="failed";native_started_=false;
  deadline_=t+20000;arm_id_=operation*8;
  std::vector<FailureEdit> edits;std::vector<unsigned> clear;
  for(const auto &entry:entries_) {
    if(entry.native){if(clearing_)clear.push_back(entry.native->id);else edits.push_back(*entry.native);continue;}
    if(entry.cdu_mask){if(!cdu_.available())throw std::runtime_error("CDU blackout service unavailable or PSX endpoint mismatch");continue;}
    if(!hwio_.available())throw std::runtime_error("HWIO failure capability unavailable or PSX endpoint mismatch");
    auto flags=hwio_.status()->data.flags;
    if(!clearing_&&entry.criterion=="ias"&&!(flags&h::ias_available))throw std::runtime_error("HWIO IAS baseline unavailable");
    if(!clearing_&&entry.criterion=="altitude"&&!(flags&h::altitude_available))throw std::runtime_error("Native corrected altitude unavailable");
  }
  if(!edits.empty()){native_.set_batch(edits,t);native_started_=true;}
  else if(!clear.empty()){native_.clear_batch(clear,t);native_started_=true;}
  advance();
}
void FailureBatch::advance() {
  while(hardware_at_<entries_.size()&&entries_[hardware_at_].native)++hardware_at_;
  if(hardware_at_==entries_.size()||hardware_failed_)return;
  if(entries_[hardware_at_].cdu_mask) {
    hardware_source_="cdu";cdu_request_=cdu_.request(entries_[hardware_at_].cdu_mask,!clearing_);
    if(!cdu_request_){hardware_failed_=true;hardware_detail_="CDU blackout unavailable; no replay";}return;
  }
  hardware_source_="hwio";
  if(!hwio_.available()){hardware_failed_=true;hardware_detail_="HWIO unavailable; no replay";return;}
  const auto &entry=entries_[hardware_at_];const auto &state=hwio_.status()->data;
  h::Packet p;p.data.revision=state.revision;
  for(unsigned i=0;i<std::min(state.arm_count,h::max_arms);++i) {
    const auto &arm=state.arms[i];if((arm.axis_mask&entry.axis_mask)||(arm.trim&&entry.trim)) {
      p.operation=h::Operation::cancel_arm;p.data.arm_id=arm.id;request_=hwio_.send(p);if(!request_){hardware_failed_=true;hardware_detail_="HWIO transmission unavailable; no replay";}return;
    }
  }
  if(clearing_||entry.criterion=="immediate") {
    p.operation=h::Operation::set;p.data.axis_mask=clearing_?state.axis_mask&~entry.axis_mask:state.axis_mask|entry.axis_mask;
    p.data.trim=entry.trim?!clearing_:state.trim!=0;
  } else {
    p.operation=h::Operation::arm;p.data.axis_mask=entry.axis_mask;p.data.trim=entry.trim;p.data.arm_id=++arm_id_;
    p.data.trigger=entry.criterion=="ias"?h::Trigger::ias:h::Trigger::native_altitude;p.data.value=entry.threshold;
  }
  request_=hwio_.send(p);if(!request_){hardware_failed_=true;hardware_detail_="HWIO transmission unavailable; no replay";}
}
void FailureBatch::pump(Operation &op,Tick t) {
  if(native_started_)native_.pump(t);
  if(cdu_request_) {
    CduBlackoutResult result;
    if(cdu_.receive(cdu_request_,result)) {
      cdu_request_=0;
      if(result.state!="accepted"){hardware_failed_=true;hardware_detail_=result.reason;hardware_state_=result.state;}
      else {
        const auto mask=entries_[hardware_at_].cdu_mask;
        hardware_results_.push_back("{\"source\":\"cdu\",\"unit\":"+quote(mask==1?"L":mask==4?"C":"R")+",\"state\":\"accepted\",\"mask\":"+std::to_string(result.mask)+",\"revision\":"+std::to_string(result.revision)+",\"render_observed\":false}");
        ++hardware_at_;advance();
      }
    } else if(t>=deadline_){cdu_request_=0;hardware_failed_=true;hardware_state_="indeterminate";hardware_detail_="CDU outcome unavailable; no replay";cdu_.disconnected();}
  }
  if(request_) {
    h::Packet reply;
    if(hwio_.receive(request_,reply)) {
      request_=0;
      if(reply.result!=h::Result::ok){hardware_failed_=true;hardware_detail_="HWIO rejected changed or unavailable preconditions; no retry";}
      else if(reply.operation!=h::Operation::cancel_arm) {
        const auto &entry=entries_[hardware_at_];const auto control=entry.trim?"trim":entry.axis_mask==h::elevator?"elevator":entry.axis_mask==h::aileron?"aileron":"rudder";
        hardware_results_.push_back("{\"source\":\"hwio\",\"control\":"+quote(control)+",\"axis_mask\":"+std::to_string(entry.axis_mask)+",\"trim\":"+(entry.trim?"true":"false")+",\"state\":\"accepted\",\"physical_application_observed\":false}");++hardware_at_;
      }
      advance();
    } else if(!hwio_.available()||t>=deadline_){request_=0;hardware_failed_=true;hardware_detail_="HWIO outcome unavailable; no replay";}
  }
  if(native_started_&&native_.busy())return;
  if(request_||cdu_request_||(!hardware_failed_&&hardware_at_<entries_.size()))return;
  bool unavailable=hardware_failed_,different=false;std::string items="[";bool comma=false;
  if(native_started_)for(const auto &item:native_.items()) {
    if(comma)items+=',';comma=true;
    items+="{\"source\":\"psx\",\"failure_id\":"+std::to_string(item.id)+",\"configured_readback\":"+quote(status(item.status))+",\"requested\":"+quote(item.expected.encode())+",\"returned\":"+(item.returned?quote(item.returned->encode()):"null")+"}";
    unavailable=unavailable||item.status==SettingsStatus::unavailable||item.status==SettingsStatus::failed;
    different=different||item.status==SettingsStatus::different;
  }
  for(const auto &item:hardware_results_){if(comma)items+=',';comma=true;items+=item;}
  if(hardware_failed_)for(std::size_t i=hardware_at_;i<entries_.size();++i)if(!entries_[i].native) {
    const auto &entry=entries_[i];const auto control=entry.trim?"trim":entry.axis_mask==h::elevator?"elevator":entry.axis_mask==h::aileron?"aileron":"rudder";
    if(comma)items+=',';comma=true;
    items+=entry.cdu_mask?"{\"source\":\"cdu\",\"unit\":"+quote(entry.cdu_mask==1?"L":entry.cdu_mask==4?"C":"R"):"{\"source\":\"hwio\",\"control\":"+quote(control);
    items+=",\"state\":"+quote(i==hardware_at_?(entry.cdu_mask?hardware_state_:"unavailable"):"not_sent")+",\"physical_application_observed\":false,\"render_observed\":false}";
  }
  op.result_json="{\"items\":"+items+"],\"hwio_error\":"+quote(hardware_source_=="hwio"?hardware_detail_:"")+",\"cdu_error\":"+quote(hardware_source_=="cdu"?hardware_detail_:"")+",\"physical_application_observed\":false,\"render_observed\":false}";
  op.state=unavailable?"indeterminate":different?"failed":"observed";
  op.detail=unavailable?"Failure batch partly sent or unavailable; no replay":different?"Configured failure readback differs; no retry":"Native configured settings observed; HWIO routing accepted; physical effects unverified";
  if(hardware_failed_&&hardware_source_=="cdu"&&!native_started_&&hardware_results_.empty()){op.state=hardware_state_;op.detail=hardware_detail_;}
}
}
