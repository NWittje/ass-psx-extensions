#pragma once
#include <psx/terrain/protocol.hpp>
#include <cstdint>
#include <variant>
#include <array>

namespace coordinator::relocation {
// Internal acquisition DTOs, not wire layouts. No client binding until PXR1 freeze.
constexpr std::uint64_t serial_limit=0x7fffffffffffffffULL;
struct Identity {std::uint32_t pid{};std::uint64_t birth{},instance{};bool operator==(const Identity&) const = default;};
struct Scope {Identity target{};std::uint32_t issuer_pid{};std::uint64_t issuer_birth{},transaction{};bool operator==(const Scope&) const = default;};
struct Domain {
 psx::motion::InstanceId producer{};
 std::uint64_t native_connection{},coordinator_connection{},outbox_generation{},owner{},pose_epoch{},terrain_epoch{};
 bool operator==(const Domain&) const = default;
};
struct Admission {
 Scope scope{};std::uint64_t token{},serial{},registry{},epoch{},edit{},deadline{};
 std::uint64_t historical_intent{},expected_intent{};
 // Original native Admission snapshot; never refreshed by worker reception/Lookup.
 std::uint64_t observation_sequence{},observed_qpc{};
 bool operator==(const Admission&) const = default;
};
struct Registered {Identity coordinator{};std::uint64_t association{},registration{};Domain domain{};bool operator==(const Registered&) const = default;};
struct Absent {bool operator==(const Absent&) const = default;};
using Writer=std::variant<Absent,Registered>; // No fabricated worker fields in absence branch.
struct Prepare {Admission admission{};Writer writer{};bool operator==(const Prepare&) const = default;};
enum class IoKind : std::uint8_t {connect,read,write};
enum class IoOutcome : std::uint8_t {idle,pending,completed,cancelled,failed,unknown};
enum class Cancel : std::uint8_t {none,requested,not_found,failed};
struct IoReceipt {
 IoKind kind{IoKind::connect};IoOutcome outcome{IoOutcome::idle};Cancel cancellation{Cancel::none};
 std::uint32_t error{},cancel_error{},requested{},completed{};
 bool issued{},query_valid{},terminal{},bytes_valid{},failure_qualified{};
 bool operator==(const IoReceipt&) const = default;
};
enum class CounterKind : std::uint8_t {source,control,query,transport};
enum class CounterState : std::uint8_t {unavailable,known,no_issued,inapplicable};
enum class Direction : std::uint8_t {none,to_native,to_coordinator};
struct CounterDomain {
 CounterKind kind{CounterKind::source};Identity issuer{};psx::motion::InstanceId incarnation{},producer{};
 std::uint64_t connection{};Direction direction{Direction::none};
 bool operator==(const CounterDomain&) const = default;
};
struct CounterPoint {
 CounterDomain domain{};std::uint64_t value{};CounterState state{CounterState::unavailable};std::uint32_t reason{};
 bool operator==(const CounterPoint&) const = default;
};
struct CounterSet {std::array<CounterPoint,4> points{};bool operator==(const CounterSet&) const = default;};
// Exact decoded native attestation, not a caller reset flag or an implemented issuer.
struct CounterReplacement {
 CounterDomain retired{},current{};Identity native_authority{};Scope scope{};
 std::uint64_t registry{},registration{},association{},binding{},consumer{},observation_sequence{},observed_qpc{};
 bool operator==(const CounterReplacement&) const = default;
};
struct WorkerReceipt {
 Prepare prepare{};std::uint64_t request_qpc{},observed_qpc{},model_generation{},producer_generation{},retired_generation{};
 std::uint64_t control_floor{},query_floor{};std::uint32_t transport_floor{},issued_mask{},completed_mask{},pending_mask{},queued{},latest{};
 std::uint32_t staged_kind{},staged_sequence{},staged_bytes{},staged_offset{};bool staged_valid{},stream_closed{},storage_safe{};
 IoReceipt connect{},read{IoKind::read},write{IoKind::write};
 CounterSet counters{}; // Source is inapplicable; three actual retired issuer/counter domains.
};
struct NativeFence {
 Prepare prepare{};std::uint64_t observed_qpc{},observation_sequence{},consumer_serial{};
 bool ingress{},polled{},final_consumer{},absence_proven{};
};
enum class Disposition : std::uint8_t {unresolved,applied,no_mutation};
enum class DispatchDisposition : std::uint8_t {unknown,never_started,finished,unresolved};
enum class ClaimDisposition : std::uint8_t {held,resolved,uncertain};
struct Resolution {
 Admission admission{};Disposition disposition{Disposition::unresolved};
 std::uint64_t epoch{},observed_qpc{},observation_sequence{},commit_qpc{},reference_sequence{},reference_qpc{};
 std::uint32_t effect_valid{},effects{};bool retained_authority{},complete_applied_proof{},dispatch_unresolved{};
 std::uint64_t current_edit{};DispatchDisposition dispatch{DispatchDisposition::unknown};ClaimDisposition claim{ClaimDisposition::uncertain};
 CounterPoint source_counter{}; // Original applicable floor; never inferred from outcome name.
};
struct Binding {
 Admission admission{};Registered writer{};std::uint64_t serial{},observed_qpc{},source_sequence_floor{},source_qpc_floor{};
 std::uint32_t hello_sequence{},subscribe_sequence{};bool native_accepted{},actual_hello_subscribe{};
 std::uint64_t observation_sequence{}; // Native service domain, distinct from PXT3 source sequence.
 // Original qualified binding barrier and actual last-excluded PXT3 watermarks.
 std::uint64_t barrier_qpc{},control_floor{},query_floor{};std::uint32_t transport_floor{};
 CounterSet requested{},accepted{},issued{},excluded{};
 std::array<CounterReplacement,4> replacements{};
};
struct Resume {
 Admission admission{};std::uint64_t binding{},current_intent{};bool current_follow{};
 Domain domain{};psx::terrain::Reference reference{};
 std::uint64_t control_floor{},query_floor{};std::uint32_t transport_floor{};
};
struct ResumeAck {Resume request{};std::uint64_t observed_qpc{};bool accepted{};std::uint64_t observation_sequence{};};
enum class Stage : std::uint8_t {idle,retiring,fenced,uncertain,resolved,bound,resuming,active,stopped};
bool serial(std::uint64_t) noexcept;
bool retired(const IoReceipt&) noexcept;
bool worker_fenced(const WorkerReceipt&) noexcept;
bool no_mutation(const Resolution&) noexcept;
bool same_reference(const psx::terrain::Reference&,const psx::terrain::Reference&) noexcept;
bool counters_correlated(const WorkerReceipt&,const NativeFence&,const Resolution&,const Binding&) noexcept;

// Acquisition owner only. IPC, OS I/O, wire encoding and formatting belong to workers.
class Model {
public:
 void configure(std::uint64_t frequency) noexcept {frequency_=frequency;}
 bool intent(bool follow) noexcept;
 bool prepare(const Prepare&,std::uint64_t now) noexcept;
 bool worker(const WorkerReceipt&,std::uint64_t now) noexcept;
 bool native(const NativeFence&,std::uint64_t now) noexcept;
 bool resolve(const Resolution&,std::uint64_t now) noexcept;
 bool binding(const Binding&,std::uint64_t now) noexcept;
 bool resume(const psx::terrain::Reference&,std::uint64_t now,Resume&) noexcept;
 bool acknowledgment(const ResumeAck&,std::uint64_t now) noexcept;
 Stage stage() const noexcept {return stage_;}
 bool inhibited() const noexcept {return stage_!=Stage::idle&&stage_!=Stage::active;}
 bool release_obsolete(Resume& exact_old_lease) noexcept {if(!release_)return false;exact_old_lease=release_request_;release_=false;return true;}
 bool current_follow() const noexcept {return follow_;}
 std::uint64_t current_intent() const noexcept {return intent_;}
private:
 void fence() noexcept;
 std::uint64_t frequency_{},intent_{1};bool follow_{},exhausted_{},worker_ready_{},native_ready_{},release_{};
 Stage stage_{Stage::idle};Prepare prepare_{};Resolution resolution_{};Binding binding_{};Resume pending_{},release_request_{};
 std::uint64_t control_floor_{},query_floor_{};std::uint32_t transport_floor_{};
 std::uint64_t native_observed_{},native_sequence_{};
 WorkerReceipt retired_worker_{};NativeFence native_fence_{};
};
}
