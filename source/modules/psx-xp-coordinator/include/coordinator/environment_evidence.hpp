#pragma once
#include "coordinator/environment.hpp"
#include <cstdint>
#include <memory>
#include <string>
namespace coordinator {
struct EnvironmentEvidenceStatus {bool active{},complete{},incomplete{};std::uint64_t records{},bytes{},overflows{};std::uint32_t error{};};
class EnvironmentEvidence {
public:
 EnvironmentEvidence();~EnvironmentEvidence();bool prepare(const std::wstring&,std::uint64_t);void begin(std::int64_t);
 void record(std::uint8_t,environment_wire::Role,const std::uint8_t*,std::size_t,std::int64_t);
 void dispatch(environment_wire::Role,const EnvironmentDispatch&,std::int64_t,std::uint8_t type=2);
 void finish(std::int64_t) noexcept;void incomplete(std::uint32_t,std::int64_t) noexcept;void join();EnvironmentEvidenceStatus status() const;
 bool done() const noexcept;
private:friend struct EvidenceTestAccess;bool test_open_hook(void (*)(void*) noexcept,void*);struct Impl;std::unique_ptr<Impl> impl_;
};
struct EnvironmentReplayResult {bool valid{},complete{};std::uint64_t qpc_frequency{},begin_qpc{},cutoff_qpc{},records{};std::array<std::uint64_t,14> kinds{};std::string error;};
// Version1 is read-only; never constructs a service or command writer.
EnvironmentReplayResult inspect_environment_capture(const std::wstring&);
}
