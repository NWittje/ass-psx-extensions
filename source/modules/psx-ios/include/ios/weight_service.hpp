#pragma once
#include "core.hpp"
#include <psx/instructor/weight_service.hpp>
#include <psx/instructor/weight_native.hpp>
namespace ios {
void validate_weight_service(std::string_view,const Object &);
class WeightService {
  psx::instructor::WeightService settings_;
  psx::instructor::PsxCommunicator &reader_;
  psx::instructor::weight_native::Client native_;
  psx::non_normal::ProcessIdentity identity_{};
  std::optional<psx::instructor::weight_native::Result> status_,reply_;
  std::optional<psx::instructor::weight_native::Command> pending_;
  enum class Phase {idle,public_set,native,wait_fuel,read_fuel};
  Phase phase_{Phase::idle};
  Tick poll_at_{},deadline_{};
  std::uint64_t request_{};int fuel_lb_{};bool fuel_seen_{},send_failed_{};
  std::optional<psx::instructor::weight_native::Command> dispatched_;
  std::uint64_t request_lifecycle_{};
  bool available(unsigned)const;
public:
  WeightService(psx::instructor::PsxCommunicator&w,psx::instructor::PsxCommunicator&r):settings_(w,r),reader_(r){}
  void poll(Tick,bool,psx::non_normal::ProcessIdentity);
  void record(std::string_view);
  void disconnected();
  void command(Operation &,Tick,bool first);
  void flush();
  std::string snapshot()const;
};
}
