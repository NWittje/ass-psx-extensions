#pragma once
#include "hwio_failures.hpp"
#include "cdu_blackout.hpp"
#include <psx/instructor/failures.hpp>
namespace ios {
struct FailureBatchEntry {
  std::optional<psx::instructor::FailureEdit> native;
  unsigned axis_mask{};
  bool trim{};
  unsigned cdu_mask{};
  std::string criterion;
  double threshold{};
};
std::vector<FailureBatchEntry> failure_batch_entries(const Object &);
class FailureBatch {
  psx::instructor::Failures &native_;
  HwioFailures &hwio_;
  CduBlackout &cdu_;
  std::vector<FailureBatchEntry> entries_;
  std::size_t hardware_at_{};
  std::uint64_t request_{},cdu_request_{},arm_id_{};
  bool clearing_{},native_started_{},hardware_failed_{};
  std::string hardware_detail_;
  std::string hardware_source_{"hwio"};
  std::string hardware_state_{"failed"};
  std::vector<std::string> hardware_results_;
  Tick deadline_{};
  void advance();
public:
  FailureBatch(psx::instructor::Failures &native,HwioFailures &hwio,CduBlackout &cdu):native_(native),hwio_(hwio),cdu_(cdu){}
  void start(const Object &,std::uint64_t operation,Tick);
  void pump(Operation &,Tick);
};
}
