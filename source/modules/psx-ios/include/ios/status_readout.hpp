#pragma once
#include <psx/instructor/ios_status_native.hpp>
#include <psx/instructor/status_display.hpp>
namespace ios {
// One bounded read request at a time on the existing acquisition worker.
class StatusReadout {
  psx::instructor::ios_status_native::Client client_;
  psx::non_normal::ProcessIdentity identity_{};
  std::optional<psx::instructor::ios_status_native::Snapshot> sample_;
  psx::instructor::Tick query_at_{},deadline_{};
  std::uint64_t request_{};
  std::int64_t minimum_qpc_{};
  bool pending_{};
  bool accept(const psx::instructor::ios_status_native::Snapshot &);
public:
  void poll(psx::instructor::Tick,bool,psx::non_normal::ProcessIdentity);
  void disconnected();
  bool available()const;
  bool loading()const{return client_.connected()&&sample_&&(sample_->flags&psx::instructor::ios_status_native::loading);}
  std::uint64_t lifecycle()const{return sample_?sample_->lifecycle:0;}
  psx::instructor::StatusDisplay::Values values(psx::instructor::Tick)const;
};
}
