#pragma once
#include "airport.hpp"
#include <psx/instructor/failures.hpp>
#include <psx/instructor/tcas.hpp>
#include <psx/instructor/runway_selection.hpp>
#include <psx/instructor/navigation_selection.hpp>
#include <psx/non_normal/client.hpp>
#include "failure_batch.hpp"
#include <psx/instructor/crash_reset.hpp>
namespace ios {
void validate_non_normal(std::string_view kind,const Object &);
std::string failure_list(const psx::instructor::Failures &,std::string_view search={});
std::string tcas_list(const psx::instructor::Tcas &);
// Owned by the existing Main worker. HTTP only queues and reads its snapshots.
class NonNormal {
  psx::instructor::PsxCommunicator &reader_;
  psx::instructor::Failures failures_;
  psx::instructor::Tcas tcas_;
  HwioFailures hwio_;
  CduBlackout cdu_;
  FailureBatch batch_;
  psx::instructor::CrashReset crash_;
  psx::instructor::RunwaySelection flight_;
  psx::non_normal::Client reset_,actor_;
  psx::non_normal::ProcessIdentity psx_{};
  std::optional<psx::non_normal::Result> reset_status_,actor_status_,reply_;
  std::wstring actor_endpoint_;
  std::uint64_t wire_request_{},query_database_{};
  psx::non_normal::Client *active_client_{};
  psx::non_normal::Command native_command_{psx::non_normal::Command::status};
  Tick connect_at_{},heartbeat_at_{},deadline_{};
  unsigned query_seen_{};
  std::optional<std::pair<psx::non_normal::Client *,psx::non_normal::Request>> pending_send_;
  enum class Phase { idle,native,reset_hwio,reset_cdu,reset_wait,reset_read,query };
  Phase phase_{Phase::idle};
  std::array<bool,psx::instructor::failure_count> reset_seen_{};
  std::uint64_t hwio_reset_request_{};
  std::string hwio_reset_json_{"null"};
  std::uint64_t cdu_reset_request_{};
  std::string cdu_reset_json_{"null"};
  bool send(psx::non_normal::Client &,psx::non_normal::Request);
  void native_finish(Operation &,const psx::non_normal::Result &);
  void begin_reset(Operation &,Tick);
  void begin_cdu_reset(Operation &,Tick);
public:
  NonNormal(psx::instructor::PsxCommunicator &writer,psx::instructor::PsxCommunicator &reader,std::wstring actor_endpoint={},std::wstring hwio_endpoint={},std::wstring cdu_endpoint={});
  void record(std::string_view,Tick);
  void disconnected();
  void abort() {crash_.cancel();}
  void poll(Tick,bool ready,psx::non_normal::ProcessIdentity,std::string host={},std::uint16_t port=0,const std::optional<psx::instructor::InstructorSample>& sample={},bool main_ready=false);
  void flush(); // Worker only, outside Runtime's snapshot mutex.
  void command(Operation &,Tick,bool first,const Catalogue *,std::uint64_t database_revision);
  std::string failures(std::string_view search={}) const {return failure_list(failures_,search);}
  std::string tcas() const {return tcas_list(tcas_);}
  std::string actor() const;
  std::string reset() const;
  std::string hwio() const {return hwio_.json();}
  std::string cdu() const {return cdu_.json();}
  std::string states() const;
};
psx::non_normal::ProcessIdentity psx_process(const psx::instructor::NavigationSelection &);
}
