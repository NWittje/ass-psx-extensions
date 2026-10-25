#pragma once
#include "coordinator/aircraft_sync.hpp"
#include <cstdint>
#include <memory>
#include <string>
namespace coordinator {
struct AircraftEvidenceStatus {bool active{},complete{},incomplete{};std::uint64_t records{},bytes{},overflows{};std::uint32_t error{};};
class AircraftEvidence {
public:
 AircraftEvidence();~AircraftEvidence();bool prepare(const std::wstring&,std::uint64_t);void begin(std::int64_t);
 void record(std::uint8_t,aircraft_wire::Role,const std::uint8_t*,std::size_t,std::int64_t);
 void dispatch(aircraft_wire::Role,const AircraftDispatch&,std::int64_t,std::uint8_t type=2);
 void finish(std::int64_t) noexcept;void incomplete(std::uint32_t,std::int64_t) noexcept;void join();AircraftEvidenceStatus status() const;
 bool done() const noexcept;
private:friend struct EvidenceTestAccess;bool test_open_hook(void (*)(void*) noexcept,void*);struct Impl;std::unique_ptr<Impl> impl_;
};
struct AircraftReplayResult {bool valid{},complete{};std::uint64_t qpc_frequency{},begin_qpc{},cutoff_qpc{},records{};std::array<std::uint64_t,9> kinds{};std::string error;};
// Version1 is read-only; never constructs a service or command writer.
AircraftReplayResult inspect_aircraft_capture(const std::wstring&);
}
