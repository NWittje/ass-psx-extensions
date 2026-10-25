#include <ios/terrain_preflight.hpp>
#include <numbers>
namespace ios {
namespace {
psx::scenery::Query query(const Operation &op) {
  const auto &m=*op.move;
  return {op.id,m.psx_pid,m.psx_birth,m.lat*180/std::numbers::pi,m.lon*180/std::numbers::pi};
}
bool same(const psx::scenery::Query &a,const psx::scenery::Query &b) {
  return a.operation==b.operation&&a.psx_pid==b.psx_pid&&a.psx_birth==b.psx_birth&&a.latitude_deg==b.latitude_deg&&a.longitude_deg==b.longitude_deg;
}
void abort(Operation &op) { op.state="aborted";op.detail="Reposition aborted"; }
}
TerrainPreflight::TerrainPreflight(Check check):check_(std::move(check)),worker_([this]{run();}) {}
TerrainPreflight::~TerrainPreflight() {
  {std::lock_guard lock(mutex_);stopping_=true;job_.reset();}
  cv_.notify_one();worker_.join();
}
void TerrainPreflight::run() {
  for(;;) {
    Job work;
    {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stopping_||job_.has_value();});if(stopping_)return;work=*job_;job_.reset();}
    psx::scenery::Result result{};result.query=work.query;result.verdict=psx::scenery::Verdict::unavailable;
    try {
      const auto t=now();
      if(t<work.deadline)result=check_?check_(work.query,std::uint32_t(work.deadline-t)):psx::scenery::check_destination(work.query,std::uint32_t(work.deadline-t));
    } catch(...) { result.query=work.query;result.verdict=psx::scenery::Verdict::unavailable; }
    const auto completed=now();
    {std::lock_guard lock(mutex_);result_=Completed{std::move(result),completed};busy_=false;}
  }
}
bool TerrainPreflight::poll(Operation &op,Tick t) {
  if(op.state=="awaiting-confirmation") {
    if(op.terrain.decision=="continue")return true;
    if(t>=op.terrain.deadline||op.terrain.decision=="abort"){abort(op);return false;}
    return false;
  }
  const auto input=query(op);
  if(op.terrain.verdict.empty()) {
    op.terrain.verdict="checking";op.terrain.deadline=t+2000;
    {std::lock_guard lock(mutex_);
      if(busy_){op.terrain.verdict="unavailable";return true;}
      result_.reset();job_=Job{input,t+2000};busy_=true;
    }
    cv_.notify_one();return false;
  }
  std::optional<Completed> result;
  {std::lock_guard lock(mutex_);if(result_){result=std::move(result_);result_.reset();}}
  if(!result){if(t<op.terrain.deadline)return false;op.terrain.verdict="unavailable";return true;}
  // A timely Missing reply remains binding if the acquisition thread was
  // delayed before collecting it. A reply completed after the deadline cannot
  // reopen an operation that has proceeded under the unavailable policy.
  if(result->at>=op.terrain.deadline||!same(input,result->result.query)){op.terrain.verdict="unavailable";return true;}
  if(result->result.verdict==psx::scenery::Verdict::missing) {
    op.terrain.verdict="missing";op.state="awaiting-confirmation";
    op.terrain.deadline=t+60000;op.detail=terrain_warning;return false;
  }
  op.terrain.verdict=result->result.verdict==psx::scenery::Verdict::available?"available":"unavailable";
  return true;
}
void TerrainPreflight::decide(Operation &op,bool proceed,std::string request,Tick t) {
  const auto decision=proceed?"continue":"abort";
  if(!op.terrain.decision.empty()) {
    if(op.terrain.decision!=decision||op.terrain.request!=request)throw std::runtime_error("Terrain decision already recorded");
    return;
  }
  if(op.state!="awaiting-confirmation")throw std::runtime_error("Terrain confirmation is no longer pending");
  if(t>=op.terrain.deadline){abort(op);return;}
  op.terrain.decision=decision;op.terrain.request=std::move(request);
  if(!proceed)abort(op);
}
}
