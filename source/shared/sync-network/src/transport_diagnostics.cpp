#include "transport_diagnostics.hpp"
#include <cstdio>
namespace psx::sync {
const char* native_call_name(NativeCall call) noexcept {
 switch(call){
#define PSX_SYNC_CALL_NAME(name) case NativeCall::name:return "Norm" #name;
 PSX_SYNC_NATIVE_CALLS(PSX_SYNC_CALL_NAME)
#undef PSX_SYNC_CALL_NAME
 default:return "none";
 }
}
const char* retirement_name(Retirement reason) noexcept {
 switch(reason){
 case Retirement::shutdown:return "shutdown";
 case Retirement::clock_expired:return "clock-expired";
 case Retirement::progress_expired:return "progress-expired";
 case Retirement::peer_identity:return "peer-identity";
 case Retirement::peer_boot:return "peer-boot";
 case Retirement::peer_connection:return "peer-connection";
 case Retirement::peer_replaced:return "peer-replaced";
 case Retirement::peer_detach:return "peer-detach";
 case Retirement::clock_discontinuity:return "clock-discontinuity";
 case Retirement::handshake_expired:return "handshake-expired";
 case Retirement::native_exception:return "native-exception";
 }return "unknown";
}
namespace {
std::array<std::uint64_t,20> words(const TransportProgress& p) noexcept {
 return {p.role,p.connection,p.generation,p.frequency,p.last_progress,p.last_clock,
 p.loop_sequence,p.loop_qpc,p.native_errors,p.command_full,p.command_size,p.command_oldest,
 p.expired,p.rejected,p.local_pid,p.peer_pid,p.local_birth,p.peer_birth,p.error,p.flags};
}
TransportProgress progress_of(const std::array<std::uint64_t,20>& p) noexcept {
 return {p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],p[16],p[17],p[18],p[19]};
}
}
void TransportDiagnostics::configure(DiagnosticSink sink,std::uint64_t run) noexcept {sink_=sink;run_=run;}
void TransportDiagnostics::progress(const TransportProgress& p) noexcept {if(enabled())progress_.store(words(p));}
void TransportDiagnostics::retire(Retirement reason,const TransportProgress& p,std::uint64_t now,std::string_view detail) noexcept {
 if(!enabled())return;Notice n;n.retirement=true;n.reason=reason;n.progress=p;n.end=now;psx::diagnostics::text(n.detail,detail);
 if(!notices_.push(n))lost_.fetch_add(1,std::memory_order_relaxed);
}
void TransportDiagnostics::service_failure(Family family,ServiceFailure failure,std::uint64_t generation,
 const TransportProgress& p,std::uint64_t now) noexcept {
 if(!enabled())return;Notice n;n.service=true;n.family=family;n.failure=failure;n.service_generation=generation;n.progress=p;n.end=now;
 if(!notices_.push(n))lost_.fetch_add(1,std::memory_order_relaxed);
}
std::uint64_t TransportDiagnostics::begin(NativeCall call,std::uint64_t& begin) noexcept {
 begin=static_cast<std::uint64_t>(native_qpc());const auto ordinal=++call_sequence_;
 call_.store({static_cast<std::uint64_t>(call),ordinal,begin,0});
 // Authored one-shot delay at the real dynamic-call boundary, never a product setting.
 if(fixture_call_.load(std::memory_order_acquire)==call&&
    fixture_call_.exchange(NativeCall::none,std::memory_order_acq_rel)==call)
  Sleep(fixture_delay_.load(std::memory_order_relaxed));
 return ordinal;
}
void TransportDiagnostics::end(NativeCall call,std::uint64_t ordinal,std::uint64_t begin,std::uint32_t error) noexcept {
 const auto end=static_cast<std::uint64_t>(native_qpc());
 call_.store({0,ordinal,begin,end});
 std::array<std::uint64_t,20> p{};
 if(progress_.load(p)&&p[3]&&end>=begin&&end-begin>=p[3]/10){
  Notice n;n.call=call;n.ordinal=ordinal;n.begin=begin;n.end=end;n.error=error;
  n.progress=progress_of(p);if(!notices_.push(n))lost_.fetch_add(1,std::memory_order_relaxed);
 }
}
void TransportDiagnostics::emit(std::string_view text,std::array<std::uint64_t,6> values,
 std::uint64_t at,const TransportProgress& p,psx::diagnostics::Severity severity,bool retain) noexcept {
 namespace d=psx::diagnostics;d::Event e;e.context=sink_.origin;e.context.run_id=run_;
 e.context.connection_id=p.connection;e.sequence=++sequence_;e.qpc=static_cast<std::int64_t>(at);
 e.producer=6;e.code=d::Code::metrics;e.component=d::Component::bridge;e.severity=severity;
 e.values=values;d::text(e.text,text);
 sink_.submit(sink_.context,e);
 if(retain&&!output_.push(e))lost_.fetch_add(1,std::memory_order_relaxed);
}
void TransportDiagnostics::observe(HANDLE stop) noexcept {
 namespace d=psx::diagnostics;std::uint64_t periodic{},active{},reported_lost{},loop_reported{};
 TransportProgress p{};
 do{
  const auto now=static_cast<std::uint64_t>(native_qpc());std::array<std::uint64_t,20> state{};
  if(progress_.load(state))p=progress_of(state);
  Notice n;
  for(unsigned budget=0;budget<64&&notices_.pop(n);++budget){
   char label[160]{};
   if(n.service){
    const char* reason=n.failure==ServiceFailure::ordered_expired?"ordered-expired":n.failure==ServiceFailure::deferred_capacity?"deferred-capacity":"admission-capacity";
    std::snprintf(label,sizeof(label),"sync.service.failure.v1 %s",reason);
    emit(label,{n.progress.role,static_cast<std::uint64_t>(n.family),static_cast<std::uint64_t>(n.failure),n.service_generation,n.end,n.progress.command_full},n.end,n.progress,d::Severity::warning);
   }else if(n.retirement){
    std::snprintf(label,sizeof(label),"sync.retire.v1 %s",retirement_name(n.reason));
    emit(label,{static_cast<std::uint64_t>(n.reason),n.progress.connection,n.progress.generation,n.end,n.progress.last_progress,n.progress.last_clock},n.end,n.progress,
         n.reason==Retirement::shutdown?d::Severity::info:d::Severity::warning);
    emit("sync.retire.queue.v1",{n.progress.role,n.progress.command_size,n.progress.command_oldest,n.progress.command_full,n.progress.expired,n.progress.native_errors},n.end,n.progress);
    std::snprintf(label,sizeof(label),"sync.retire.error.v1 %.130s",n.detail.data());
    emit(label,{n.progress.role,n.progress.error,n.progress.flags,n.progress.native_errors,n.progress.rejected,n.progress.expired},n.end,n.progress);
   }else{
    std::snprintf(label,sizeof(label),"sync.norm.done.v1 %s",native_call_name(n.call));
    emit(label,{n.progress.role,static_cast<std::uint64_t>(n.call),n.ordinal,n.begin,n.end,n.error},n.end,n.progress,d::Severity::warning);
   }
  }
  std::array<std::uint64_t,4> call{};
  if(call_.load(call)&&call[0]&&p.frequency&&now>=call[2]&&now-call[2]>=p.frequency/10&&active!=call[1]){
   char label[160]{};std::snprintf(label,sizeof(label),"sync.norm.active.v1 %s",native_call_name(static_cast<NativeCall>(call[0])));
   emit(label,{p.role,call[0],call[1],call[2],now,now-call[2]},now,p,d::Severity::warning);active=call[1];
  }
  if(p.frequency&&p.loop_qpc&&now>=p.loop_qpc&&now-p.loop_qpc>=p.frequency/10&&loop_reported!=p.loop_sequence){
   emit("sync.loop.gap.v1",{p.role,p.loop_sequence,p.loop_qpc,now,call[0],call[1]},now,p,d::Severity::warning);loop_reported=p.loop_sequence;
  }
  if(p.frequency&&(!periodic||now-periodic>=p.frequency)){
   emit("sync.progress.v1",{p.role,p.connection,p.generation,p.last_progress,p.last_clock,now},now,p,d::Severity::info,false);
   emit("sync.queue.v1",{p.role,p.command_size,p.command_oldest,p.command_full,p.expired,p.native_errors},now,p,d::Severity::info,false);
   emit("sync.identity.v1",{p.role,p.local_pid,p.peer_pid,p.local_birth,p.peer_birth,p.frequency},now,p,d::Severity::info,false);
   periodic=now;
  }
  const auto lost=lost_.load(std::memory_order_relaxed);
  if(lost!=reported_lost){emit("sync.diagnostics.lost.v1",{p.role,lost,reported_lost,0,0,0},now,p,d::Severity::warning,false);reported_lost=lost;}
 }while(WaitForSingleObject(stop,25)==WAIT_TIMEOUT);
}
}
