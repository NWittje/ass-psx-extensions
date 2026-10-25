#include <psx/instructor/crash_reset.hpp>
#include <stdexcept>
namespace psx::instructor {
void CrashReset::finish(std::string state,std::string detail) {phase_=Phase::idle;state_=std::move(state);detail_=std::move(detail);}
void CrashReset::record(std::string_view line) {
  if(line=="load1"||line=="exit"){disconnected();return;}
  if(!line.starts_with("Qi129="))return;
  mask_.reset();auto n=integer(line.substr(6));if(n&&*n>=0&&*n<=3)mask_=*n;
  if((phase_==Phase::pausing||phase_==Phase::observing)&&mask_&&*mask_==finish_mask_)final_observed_=true;
  if(phase_==Phase::observing&&final_observed_)finish("observed","Final full-pause register observed; physical crash recovery unverified");
}
void CrashReset::start(Tick t) {
  if(busy())throw std::logic_error("Crash reset sender busy");
  if(!mask_)throw std::invalid_argument("PSX pause mask unavailable");
  finish_mask_=1|(*mask_&2);final_observed_=false;phase_=Phase::sending;deadline_=t+5000;
  state_="sent";detail_="Running PSX with Motion Freeze for one second";
  if(!writer_.send("Qi129=2\n"))finish("indeterminate","Crash-reset transmission uncertain; no replay");
}
void CrashReset::pump(Tick t) {
  if(!busy())return;
  if(writer_.state()!=network::State::connected){disconnected();return;}
  if(phase_==Phase::sending&&!writer_.sending()){phase_=Phase::running;deadline_=t+1000;}
  else if(phase_==Phase::running&&t>=deadline_) {
    phase_=Phase::pausing;deadline_=t+5000;detail_="Requesting full pause, retaining entry Motion Freeze";
    if(!writer_.send("Qi129="+std::to_string(finish_mask_)+"\n"))finish("indeterminate","Final pause transmission uncertain; no replay");
  } else if(phase_==Phase::pausing&&!writer_.sending()) {
    phase_=Phase::observing;deadline_=t+5000;
    // A forwarded observation can arrive during transmission completion.
    if(final_observed_)finish("observed","Final full-pause register observed; physical crash recovery unverified");
  } else if(phase_!=Phase::running&&t>=deadline_)finish("indeterminate","Crash-reset send or register observation unavailable; no replay");
}
void CrashReset::cancel() {
  if(busy()) {
    if(writer_.state()==network::State::connected)writer_.send("Qi129="+std::to_string(finish_mask_)+"\n");
    finish("indeterminate","Crash reset interrupted; final pause requested where connected; no replay");
  }
}
void CrashReset::disconnected() {mask_.reset();if(busy())finish("indeterminate","Disconnected during crash reset; final pause unavailable; no replay");}
}
