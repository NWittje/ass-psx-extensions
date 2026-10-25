#pragma once
#include <psx/environment/protocol.hpp>
#include <array>
#include <cstring>
namespace coordinator {
// Fixed metadata only: no weather values, allocation, formatting or I/O.
struct EnvironmentDiagnosticInput {psx::environment::Kind kind{};std::uint32_t groups{};std::array<std::uint64_t,6> head{},scope{};};
struct EnvironmentDiagnosticRecord {EnvironmentDiagnosticInput input{};std::uint64_t observations{},coalesced{};bool dirty{};};
class EnvironmentDiagnosticTrace {
public:
 void record(const EnvironmentDiagnosticInput& input,unsigned admission) noexcept {
  const unsigned kind=input.kind==psx::environment::Kind::ack?0u:input.kind==psx::environment::Kind::applied?1u:2u;
  if(kind==2||admission>2||!input.groups)return;
  for(unsigned g=0;g<2;++g)if(input.groups&(1u<<g)){auto& s=slots_[(kind*3+admission)*2+g];++received_;++s.observations;if(s.dirty){++s.coalesced;++coalesced_;}s.input=input;s.input.head[2]|=std::uint64_t(admission)<<40;s.dirty=true;}
 }
 bool due(std::int64_t now,std::uint64_t frequency) noexcept {if(!frequency||now<last_||(last_&&std::uint64_t(now-last_)<frequency))return false;last_=now;remaining_=8;return true;}
 bool next(EnvironmentDiagnosticRecord& record,unsigned& index) noexcept {
  if(!remaining_)return false;
  for(unsigned n=0;n<slots_.size();++n){const unsigned i=cursor_;cursor_=(cursor_+1)%static_cast<unsigned>(slots_.size());if(slots_[i].dirty){record=slots_[i];index=i;slots_[i].dirty=false;--remaining_;return true;}}return false;
 }
 std::uint64_t received() const noexcept{return received_;}
 std::uint64_t coalesced() const noexcept{return coalesced_;}
 std::uint64_t pending() const noexcept{std::uint64_t n{};for(const auto& s:slots_)n+=s.dirty?1u:0u;return n;}
private:
 std::array<EnvironmentDiagnosticRecord,12> slots_{};unsigned cursor_{},remaining_{};std::uint64_t received_{},coalesced_{};std::int64_t last_{};
};
inline EnvironmentDiagnosticInput environment_ack_input(const psx::environment::Ack& a,std::int64_t received) noexcept {
 const auto& c=a.control;return {psx::environment::Kind::ack,c.groups,
  {static_cast<std::uint64_t>(received),c.operation,std::uint64_t(c.groups)|(std::uint64_t(c.action)<<8)|(std::uint64_t(a.phase)<<16)|(std::uint64_t(a.reason)<<24),a.accepted_groups,c.lifecycle,c.configuration},
  {c.producer.high,c.producer.low,c.owner,c.duration_ms,0,0}};
}
inline EnvironmentDiagnosticInput environment_applied_input(const psx::environment::Applied& a,std::int64_t received) noexcept {
 return {psx::environment::Kind::applied,a.groups,
  {static_cast<std::uint64_t>(received),a.source.sequence,std::uint64_t(a.groups)|(std::uint64_t(a.phase)<<16)|(std::uint64_t(a.reason)<<24),a.accepted_mask,a.actual_mask,a.location_mask},
  {a.source.lifecycle,a.source.configuration,static_cast<std::uint64_t>(a.source.sample_end_qpc),static_cast<std::uint64_t>(a.applied_qpc),a.owner,a.provider_generation}};
}
// Single coalesced transition slot; original rates/clock fields are not a sample trace.
struct EnvironmentTimeTransition {std::array<std::uint64_t,6> head{},scope{};};
class EnvironmentTimeTransitions {
public:
 void record(std::array<std::uint64_t,10> key,std::int64_t now) noexcept {
  if(known_&&key==key_)return;if(dirty_)++coalesced_;++transitions_;key_=key;known_=dirty_=true;observed_=now;
 }
 bool next(EnvironmentTimeTransition& out) noexcept {
  if(!dirty_)return false;out.head={static_cast<std::uint64_t>(observed_),key_[0],key_[1],key_[9],transitions_,coalesced_};
  out.scope={key_[2],key_[3],key_[4],key_[5],key_[6],key_[7]};dirty_=false;return true;
 }
 std::uint64_t transitions() const noexcept{return transitions_;}
private:
 std::array<std::uint64_t,10> key_{};std::int64_t observed_{};std::uint64_t transitions_{},coalesced_{};bool known_{},dirty_{};
};
}
