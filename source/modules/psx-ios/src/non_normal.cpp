#include <ios/non_normal.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
namespace ios {
namespace {
using namespace psx::instructor;
namespace ipc=psx::non_normal;
std::string value(const Object &o) {
  auto it=o.find("value");if(it==o.end())throw std::invalid_argument("Missing criterion value");
  if(auto s=std::get_if<std::string>(&it->second))return *s;
  double v=numeric(o,"value");if(std::trunc(v)!=v||std::abs(v)>10000000)throw std::invalid_argument("Criterion value must be an integer");
  return std::to_string(static_cast<int>(v));
}
FailureEdit edit(const Object &o) {
  auto id=field<std::uint64_t>(o,"failure_id");if(id>UINT_MAX)throw std::invalid_argument("Unknown failure");
  return {static_cast<unsigned>(id),field<bool>(o,"severe"),failure_criterion(field<std::string>(o,"criterion")),value(o)};
}
std::string phase(ipc::Phase p) {
  switch(p) {case ipc::Phase::unavailable:return "unavailable";case ipc::Phase::accepted:return "accepted";case ipc::Phase::loading:return "loading";case ipc::Phase::applied:return "sdk_submitted";case ipc::Phase::action_returned:return "native_action_returned";case ipc::Phase::rejected:return "rejected";case ipc::Phase::indeterminate:return "indeterminate";case ipc::Phase::removed:return "removed";}return "unknown";
}
std::string reason(ipc::Reason r) {
  constexpr const char *names[]{"none","unsupported","identity","expired","disconnected","busy","malformed","binding","native_exception","model","terrain","scenery","replaced","revision","transport"};
  auto i=static_cast<unsigned>(r);return i<std::size(names)?names[i]:"unknown";
}
std::string native_json(const std::optional<ipc::Result> &r,bool connected,unsigned capability) {
  return "{\"available\":"+std::string(connected&&r&&(r->capabilities&capability)?"true":"false")+
    ",\"connected\":"+(connected?"true":"false")+",\"state\":"+quote(r?(capability==ipc::reset_capability&&r->phase==ipc::Phase::applied?"ready":phase(r->phase)):"unavailable")+
    ",\"reason\":"+quote(connected?(r?reason(r->reason):"Waiting for service status"):"Required local service unavailable")+
    ",\"revision\":"+std::to_string(r?r->revision:0)+",\"scenery_generation\":"+std::to_string(r?r->scenery_generation:0)+
    ",\"resource_generation\":"+std::to_string(r?r->resource_generation:0)+
    ",\"latitude\":"+number(r?r->latitude_deg:0)+",\"longitude\":"+number(r?r->longitude_deg:0)+
    ",\"nose_contact_msl_m\":"+number(r?r->altitude_m:0)+",\"heading_true\":"+number(r?r->heading_deg:0)+
    ",\"pitch\":"+number(r?r->pitch_deg:0)+",\"bank\":"+number(r?r->bank_deg:0)+"}";
}
void settings_finish(Operation &op,const SettingsResult &r) {
  op.detail=r.detail;
  op.state=r.status==SettingsStatus::match||r.status==SettingsStatus::activated||r.status==SettingsStatus::different?"observed":
    r.status==SettingsStatus::pending?"sent":r.status==SettingsStatus::failed?"indeterminate":"indeterminate";
  const auto label=r.status==SettingsStatus::match?"matching":r.status==SettingsStatus::activated?"activated":r.status==SettingsStatus::different?"different":r.status==SettingsStatus::pending?"pending":"unavailable";
  op.result_json="{\"configured_readback\":"+quote(label)+",\"requested\":"+quote(r.requested)+",\"returned\":"+quote(r.returned)+",\"physical_application_observed\":false}";
}
}
void validate_non_normal(std::string_view kind,const Object &o) {
  if(kind=="failure-batch") {failure_batch_entries(o);}
  else if(kind=="failure-set") {keys(o,{"request_id","expected_revision","failure_id","severe","criterion","value"});validate_failure_edit(edit(o));}
  else if(kind=="failure-clear") {keys(o,{"request_id","expected_revision","failure_id"});auto n=field<std::uint64_t>(o,"failure_id");if(n>UINT_MAX)throw std::invalid_argument("Unknown failure");failure_info(static_cast<unsigned>(n));}
  else if(kind=="tcas") {keys(o,{"request_id","expected_revision","preset"});tcas_preset(field<std::string>(o,"preset"));}
  else if(kind=="runway-place") {
    keys(o,{"request_id","expected_revision","database_revision","actor_revision","airport","runway"});
    airport_id(field<std::string>(o,"airport"));auto r=field<std::string>(o,"runway");
    if(r.size()<2||r.size()>3||r.find_first_not_of("0123456789LCR")!=r.npos)throw std::invalid_argument("Invalid runway end");
    field<std::uint64_t>(o,"database_revision");field<std::uint64_t>(o,"actor_revision");
  } else if(kind=="runway-remove") {keys(o,{"request_id","expected_revision","actor_revision"});field<std::uint64_t>(o,"actor_revision");}
  else if(kind=="failure-reset-all"||kind=="runway-candidates"||kind=="crash-reset") keys(o,{"request_id","expected_revision"});
  else throw std::invalid_argument("Unknown non-normal command");
  field<std::string>(o,"request_id");field<std::uint64_t>(o,"expected_revision");
}
std::string failure_list(const Failures &f,std::string_view search) {
  auto ids=search_failures(search);std::string out="{\"ordinary_count\":"+std::to_string(failure_count)+",\"base_count\":184,\"failures\":[";
  bool comma=false;
  for(auto id:ids) {const auto &i=failure_info(id);if(comma)out+=',';comma=true;
    out+="{\"id\":"+std::to_string(id)+",\"name\":"+quote(i.name)+",\"category\":"+quote(i.category)+",\"severe_variant\":"+quote(i.severe)+",\"non_severe_variant\":"+quote(i.non_severe)+
      ",\"criteria\":[\"ias\",\"altitude\",\"heading\",\"distance\",\"utc_time\",\"random\",\"immediate\"],\"configured\":";
    const auto &s=f.setting(id);if(!s)out+="null";else out+="{\"severe\":"+std::string(s->severe?"true":"false")+",\"criterion\":"+quote(criterion_name(s->criterion))+",\"value\":"+quote(s->value)+",\"state\":"+quote(s->off()?"off":s->armed()?"armed":"active")+"}";
    out+='}';
  }return out+"]}";
}
std::string tcas_list(const Tcas &t) {
  std::string out="{\"mode\":"+(t.mode()?std::to_string(*t.mode()):"null")+",\"presets\":[";bool comma=false;
  for(const auto &p:tcas_presets()) {if(comma)out+=',';comma=true;out+="{\"id\":"+quote(p.id)+",\"name\":"+quote(p.name)+",\"normalized_mode\":"+std::to_string(p.mode)+"}";}
  return out+"]}";
}
NonNormal::NonNormal(PsxCommunicator &writer,PsxCommunicator &reader,std::wstring endpoint,std::wstring hwio_endpoint,std::wstring cdu_endpoint):reader_(reader),failures_(writer,reader),tcas_(writer,reader),hwio_(std::move(hwio_endpoint)),cdu_(std::move(cdu_endpoint)),batch_(failures_,hwio_,cdu_),crash_(writer),actor_endpoint_(std::move(endpoint)) {}
bool NonNormal::send(ipc::Client &c,ipc::Request r) {r.request=c.next_request();if(!r.request)return false;wire_request_=r.request;active_client_=&c;native_command_=r.command;reply_.reset();pending_send_=std::pair{&c,r};return true;}
void NonNormal::flush() {if(pending_send_){auto [client,request]=*pending_send_;pending_send_.reset();client->send(request,0);}}
void NonNormal::disconnected() {
  crash_.disconnected();hwio_.disconnected();cdu_.disconnected();failures_.disconnected();tcas_.disconnected();flight_.reset();reset_.disconnect();actor_.disconnect();reset_status_.reset();actor_status_.reset();reply_.reset();pending_send_.reset();phase_=Phase::idle;psx_={};connect_at_=0;
}
void NonNormal::record(std::string_view line,Tick t) {
  if(line=="load1"||line=="exit") {disconnected();return;}
  failures_.record(line);tcas_.record(line);flight_.record(line,t);crash_.record(line);
  if(phase_==Phase::query) {
    if(line.starts_with("Qs121="))query_seen_|=1;
    if(line.starts_with("Qs373="))query_seen_|=2;
    if(line.starts_with("Qs376="))query_seen_|=4;
    if(line.starts_with("Qs377="))query_seen_|=8;
    if(line.starts_with("Qs493="))query_seen_|=16;
  }
  if(phase_==Phase::reset_read && line.starts_with("Qs")) {
    auto p=line.find('=');unsigned id{};if(p==line.npos)return;
    auto r=std::from_chars(line.data()+2,line.data()+p,id);if(r.ec!=std::errc{}||r.ptr!=line.data()+p)return;
    auto all=failure_catalogue();auto i=std::lower_bound(all.begin(),all.end(),id,[](const auto &a,unsigned n){return a.id<n;});
    if(i!=all.end()&&i->id==id && failures_.setting(id))reset_seen_[static_cast<std::size_t>(i-all.begin())]=true;
  }
}
void NonNormal::poll(Tick t,bool ready,ipc::ProcessIdentity identity,std::string host,std::uint16_t port,const std::optional<InstructorSample>&sample,bool main_ready) {
  // Establish readiness before transferring the endpoint string into the HWIO client.
  const bool main_endpoint_ready=main_ready&&!host.empty()&&port!=0;
  cdu_.poll(t,main_endpoint_ready,host,port);
  hwio_.poll(t,main_endpoint_ready,std::move(host),port,sample);
  if(!ready) {if(reset_.is_connected()||actor_.is_connected())disconnected();return;}
  if(identity!=psx_) {reset_.disconnect();reset_status_.reset();psx_=identity;connect_at_=0;}
  if(t>=connect_at_) {
    connect_at_=t+1000;
    ipc::Request r;r.command=ipc::Command::status;
    if(!reset_.is_connected()&&identity.pid&&reset_.connect(ipc::reset_endpoint(identity),identity)) {r.request=reset_.next_request();reset_.send(r,0);}
    if(!actor_.is_connected()&&actor_.connect(actor_endpoint_.empty()?ipc::coordinator_endpoint():actor_endpoint_)) {r.wire_version=ipc::actor_version;r.request=actor_.next_request();actor_.send(r,0);heartbeat_at_=t+250;}
  }
  for(auto pair:{std::pair{&reset_,&reset_status_},std::pair{&actor_,&actor_status_}}) {
    ipc::Result r;unsigned count=0;
    while(count++<32 && pair.first->receive(r,0)) {
      *pair.second=r;
      if(phase_==Phase::native&&pair.first==active_client_&&r.request==wire_request_&&r.command==native_command_) reply_=r;
    }
    if(!pair.first->is_connected())pair.second->reset();
  }
  if(t>=heartbeat_at_) {
    for(auto *client:{&reset_,&actor_})if(client->is_connected()) {ipc::Request r;r.wire_version=client==&actor_?ipc::actor_version:ipc::version;r.command=ipc::Command::heartbeat;r.request=client->next_request();client->send(r,0);}
    heartbeat_at_=t+250;
  }
}
std::string NonNormal::actor() const {return native_json(actor_status_,actor_.is_connected(),ipc::actor_capability);}
std::string NonNormal::reset() const {return native_json(reset_status_,reset_.is_connected(),ipc::reset_capability);}
std::string NonNormal::states() const {
  std::string s="[";bool comma=false;
  for(const auto &f:failure_catalogue())if(const auto &setting=failures_.setting(f.id);setting) {
    if(comma)s+=',';comma=true;s+="{\"id\":"+std::to_string(f.id)+",\"severe\":"+(setting->severe?"true":"false")+",\"criterion\":"+quote(criterion_name(setting->criterion))+",\"value\":"+quote(setting->value)+",\"state\":"+quote(setting->off()?"off":setting->armed()?"armed":"active")+"}";
  }
  return s+"]";
}
void NonNormal::begin_cdu_reset(Operation &op,Tick t) {
  cdu_reset_json_="{\"state\":\"unavailable\",\"reason\":\"CDU blackout service unavailable\",\"render_observed\":false}";
  if(cdu_.available()&&(cdu_reset_request_=cdu_.request(0,false,true))!=0){phase_=Phase::reset_cdu;deadline_=t+6000;return;}
  begin_reset(op,t);
}
void NonNormal::begin_reset(Operation &op,Tick t) {
  op.result_json="{\"hwio_clear\":"+hwio_reset_json_+",\"cdu_clear\":"+cdu_reset_json_+",\"native_action_returned\":false}";
  if(!reset_.is_connected()||!reset_status_||!(reset_status_->capabilities&ipc::reset_capability)) {
    op.state="failed";op.detail="HWIO cleanup attempted; native Reset All service unavailable";
    op.result_json="{\"hwio_clear\":"+hwio_reset_json_+",\"cdu_clear\":"+cdu_reset_json_+",\"native_action_returned\":false,\"native_error\":\"Native Reset All service unavailable\"}";phase_=Phase::idle;return;
  }
  ipc::Request r;r.command=ipc::Command::reset_malfunctions;phase_=Phase::native;deadline_=t+5000;
  if(!send(reset_,r)){op.state="indeterminate";op.detail="Native reset transmission uncertain; no replay";phase_=Phase::idle;}
}
void NonNormal::native_finish(Operation &op,const ipc::Result &r) {
  auto evidence=native_json(r,true,op.kind=="failure-reset-all"?ipc::reset_capability:ipc::actor_capability);
  op.result_json=op.kind=="failure-reset-all"?"{\"hwio_clear\":"+hwio_reset_json_+",\"cdu_clear\":"+cdu_reset_json_+",\"native_action_returned\":"+(r.phase==ipc::Phase::action_returned?"true":"false")+",\"native\":"+evidence+"}":evidence;
  if(r.phase==ipc::Phase::accepted||r.phase==ipc::Phase::loading) {op.detail=phase(r.phase);return;}
  if(r.phase==ipc::Phase::action_returned) {phase_=Phase::reset_wait;deadline_=now()+1000;op.detail="Native reset action returned; waiting for configured readback";return;}
  op.state=r.phase==ipc::Phase::applied||r.phase==ipc::Phase::removed?"observed":r.phase==ipc::Phase::indeterminate?"indeterminate":r.reason==ipc::Reason::replaced||r.reason==ipc::Reason::revision?"superseded":"failed";
  op.detail=phase(r.phase)+": "+reason(r.reason);phase_=Phase::idle;
}
void NonNormal::command(Operation &op,Tick t,bool first,const Catalogue *catalogue,std::uint64_t revision) {
  try {
    const auto &o=*op.command;
    if(first) {
      phase_=Phase::idle;reply_.reset();deadline_=t+5000;
      if(op.kind=="failure-batch")batch_.start(o,op.id,t);
      else if(op.kind=="crash-reset")crash_.start(t);
      else if(op.kind=="failure-reset-all") {
        hwio_reset_json_="{\"state\":\"unavailable\",\"reason\":\"HWIO service unavailable\"}";
        if(hwio_.available()) {
          psx::hwio::failures::Packet r;r.operation=psx::hwio::failures::Operation::clear_all;r.data.revision=hwio_.status()->data.revision;
          hwio_reset_request_=hwio_.send(r);phase_=Phase::reset_hwio;deadline_=t+2000;
        } else begin_cdu_reset(op,t);
      }
      else if(op.kind=="failure-set") failures_.set(edit(o),t);
      else if(op.kind=="failure-clear") failures_.clear(static_cast<unsigned>(field<std::uint64_t>(o,"failure_id")),t);
      else if(op.kind=="tcas") tcas_.select(field<std::string>(o,"preset"),t);
      else if(op.kind=="runway-candidates") {
        if(!catalogue)throw std::runtime_error("Active PSX navigation database unavailable");
        phase_=Phase::query;query_seen_=0;query_database_=revision;
        if(!reader_.send("bang\n"))throw std::runtime_error("Candidate read request unsuccessful");
      } else {
        bool reset=op.kind=="failure-reset-all";auto &client=reset?reset_:actor_;auto &status=reset?reset_status_:actor_status_;
        if(!client.is_connected()||!status||!(status->capabilities&(reset?ipc::reset_capability:ipc::actor_capability)))throw std::runtime_error(std::string(reset?"Native Reset All service unavailable":"Coordinator runway aircraft capability unavailable")+(status?": "+reason(status->reason):""));
        ipc::Request r;r.command=reset?ipc::Command::reset_malfunctions:op.kind=="runway-place"?ipc::Command::place_actor:ipc::Command::remove_actor;
        if(!reset)r.wire_version=ipc::actor_version;
        if(!reset)r.expected_revision=field<std::uint64_t>(o,"actor_revision");
        if(op.kind=="runway-place") {
          if(!catalogue||revision!=field<std::uint64_t>(o,"database_revision"))throw std::runtime_error("Navigation database changed; query candidates again");
          auto airport=airport_id(field<std::string>(o,"airport"));auto runway=field<std::string>(o,"runway");
          const auto &a=catalogue->airport(airport);auto i=std::find_if(a.runways.begin(),a.runways.end(),[&](const auto &v){return v.id==runway;});
          if(i==a.runways.end())throw std::runtime_error("Runway not found");
          r.database_revision=revision;r.latitude_deg=i->threshold.lat*180/std::numbers::pi;r.longitude_deg=i->threshold.lon*180/std::numbers::pi;r.heading_deg=wrap_degrees(i->heading*180/std::numbers::pi);
          if(i->elevation<INT16_MIN||i->elevation>INT16_MAX)throw std::runtime_error("Runway-end elevation out of range");
          r.runway_elevation_ft=std::int16_t(i->elevation);r.runway_elevation_valid=true;
          r.psx=psx_;
          std::copy(airport.begin(),airport.end(),r.airport.begin());std::copy(runway.begin(),runway.end(),r.runway.begin());
        }
        phase_=Phase::native;if(!send(client,r)){op.state="indeterminate";op.detail="Native transmission uncertain; no replay";phase_=Phase::idle;return;}
      }
    }
    if(op.kind=="failure-batch"){batch_.pump(op,t);return;}
    if(op.kind=="crash-reset") {crash_.pump(t);op.state=crash_.state();op.detail=crash_.detail();op.target=crash_.finish_mask();op.result_json="{\"register_observed\":"+std::string(op.state=="observed"?"true":"false")+",\"physical_recovery_observed\":false}";return;}
    if(op.kind=="failure-set"||op.kind=="failure-clear") {failures_.pump(t);settings_finish(op,failures_.result());return;}
    if(op.kind=="tcas") {tcas_.pump(t);settings_finish(op,tcas_.result());return;}
    if(phase_==Phase::reset_hwio) {
      psx::hwio::failures::Packet result;
      if(hwio_.receive(hwio_reset_request_,result)) {hwio_reset_json_="{\"state\":"+quote(result.result==psx::hwio::failures::Result::ok?"accepted":"rejected")+",\"physical_application_observed\":false}";begin_cdu_reset(op,t);}
      else if(!hwio_.available()||t>=deadline_||!hwio_reset_request_) {hwio_reset_json_="{\"state\":\"unavailable\",\"reason\":\"HWIO cleanup outcome unavailable; no replay\"}";begin_cdu_reset(op,t);}
      if(op.terminal())return;
    }
    if(phase_==Phase::reset_cdu) {
      CduBlackoutResult result;
      if(cdu_.receive(cdu_reset_request_,result)){cdu_reset_json_=result.json();begin_reset(op,t);}
      else if(t>=deadline_){cdu_.disconnected();cdu_reset_json_="{\"state\":\"indeterminate\",\"reason\":\"CDU cleanup outcome unavailable; no replay\",\"render_observed\":false}";begin_reset(op,t);}
      if(op.terminal())return;
    }
    if(phase_==Phase::native) {
      if(reply_) {auto r=*reply_;reply_.reset();native_finish(op,r);if(op.terminal())return;}
      if(phase_==Phase::native && (t>=deadline_ || !(op.kind=="failure-reset-all"?reset_:actor_).is_connected())) {op.state="indeterminate";op.detail="Native outcome unavailable; no replay";phase_=Phase::idle;}
    }
    if(phase_==Phase::reset_wait && t>=deadline_) {reset_seen_={};phase_=Phase::reset_read;deadline_=t+5000;if(!reader_.send("bang\n")){deadline_=t;}}
    if(phase_==Phase::reset_read && (std::all_of(reset_seen_.begin(),reset_seen_.end(),[](bool b){return b;})||t>=deadline_)) {
      bool complete=std::all_of(reset_seen_.begin(),reset_seen_.end(),[](bool b){return b;});std::string different;unsigned count=0;
      for(const auto &f:failure_catalogue()) {auto &s=failures_.setting(f.id);if(s&&!s->off()){if(count++)different+=',';different+=std::to_string(f.id);}}
      op.state="observed";op.detail="Native reset action returned; configured readback "+std::string(!complete?"unavailable":count?"differs":"matches clear");
      op.result_json="{\"hwio_clear\":"+hwio_reset_json_+",\"cdu_clear\":"+cdu_reset_json_+",\"native_action_returned\":true,\"configured_readback\":"+quote(!complete?"unavailable":count?"different":"matching")+",\"not_clear\":["+different+"],\"physical_repair_observed\":false}";phase_=Phase::idle;
    }
    if(phase_==Phase::query) {
      if(revision!=query_database_||!catalogue) {op.state="superseded";op.detail="Navigation database changed during query";phase_=Phase::idle;return;}
      if(query_seen_==31) {
        if(!flight_.position())throw std::runtime_error("Aircraft position unavailable");
        std::vector<RunwayOption> all;
        for(const auto &[id,a]:catalogue->airports)for(const auto &r:a.runways)all.push_back({id,r.id,r.threshold,r.heading});
        auto [airport,runway]=flight_.fmc();auto track=flight_.track(t);auto selected=runway_candidates(all,*flight_.position(),track,airport,runway);
        op.result_json="{\"database_revision\":"+std::to_string(revision)+",\"ground_track_available\":"+(track?"true":"false")+",\"candidates\":[";bool comma=false;
        for(const auto &r:selected) {if(comma)op.result_json+=',';comma=true;op.result_json+="{\"airport\":"+quote(r.airport)+",\"runway\":"+quote(r.runway)+",\"distance_nm\":"+number(r.distance_nm)+",\"heading_true\":"+number(wrap_degrees(r.heading*180/std::numbers::pi))+",\"preselected\":"+(r.fmc?"true":"false")+"}";}
        op.result_json+="]}";op.state="observed";op.detail=selected.empty()?"No eligible runway; FMC arrival runway unavailable":"Runway candidates resolved";phase_=Phase::idle;
      } else if(t>=deadline_) {op.state="failed";op.detail="Aircraft/FMC readback unavailable within five seconds";phase_=Phase::idle;}
    }
  } catch(const std::exception &e) {op.state=first?"failed":"indeterminate";op.detail=e.what();phase_=Phase::idle;failures_.cancel();tcas_.cancel();}
}
ipc::ProcessIdentity psx_process(const NavigationSelection &selection) {
  if(!selection.pid)return {};
  HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,selection.pid);if(!p)return {};
  FILETIME created{},exit{},kernel{},user{};bool good=GetProcessTimes(p,&created,&exit,&kernel,&user)!=FALSE;CloseHandle(p);
  ULARGE_INTEGER birth{};birth.LowPart=created.dwLowDateTime;birth.HighPart=created.dwHighDateTime;
  if(!good||birth.QuadPart/10000-11644473600000ULL!=selection.process_start_unix_ms)return {};
  return {selection.pid,birth.QuadPart};
}
}
