#pragma once
#include <psx/sync/diagnostics.hpp>
#include <psx/sync/protocol.hpp>
#include <array>
#include <atomic>
#include <windows.h>
namespace psx::sync {
std::int64_t native_qpc() noexcept;
template<class T,std::size_t N> class DiagnosticRing {
 std::array<T,N> values_{};
 alignas(64) std::atomic<std::uint64_t> head_{};
 alignas(64) std::atomic<std::uint64_t> tail_{};
public:
 bool push(const T& value) noexcept {
  const auto tail=tail_.load(std::memory_order_relaxed);
  if(tail-head_.load(std::memory_order_acquire)==N)return false;
  values_[tail%N]=value;tail_.store(tail+1,std::memory_order_release);return true;
 }
 bool pop(T& value) noexcept {
  const auto head=head_.load(std::memory_order_relaxed);
  if(head==tail_.load(std::memory_order_acquire))return false;
  value=values_[head%N];head_.store(head+1,std::memory_order_release);return true;
 }
};
// Each field is atomic: a bounded version check must never race a plain C++ read.
template<std::size_t N> class DiagnosticSnapshot {
 std::atomic<std::uint64_t> version_{};
 std::array<std::atomic<std::uint64_t>,N> values_{};
public:
 void store(const std::array<std::uint64_t,N>& value) noexcept {
  version_.fetch_add(1,std::memory_order_acq_rel);
  for(std::size_t i=0;i<N;++i)values_[i].store(value[i],std::memory_order_relaxed);
  version_.fetch_add(1,std::memory_order_release);
 }
 bool load(std::array<std::uint64_t,N>& value) const noexcept {
  for(unsigned attempt=0;attempt<3;++attempt){
   const auto before=version_.load(std::memory_order_acquire);if(before&1)continue;
   for(std::size_t i=0;i<N;++i)value[i]=values_[i].load(std::memory_order_relaxed);
   std::atomic_thread_fence(std::memory_order_acquire);
   if(version_.load(std::memory_order_relaxed)==before)return true;
  }return false;
 }
};
struct TransportProgress {
 std::uint64_t role{},connection{},generation{},frequency{},last_progress{},last_clock{};
 std::uint64_t loop_sequence{},loop_qpc{},native_errors{},command_full{},command_size{},command_oldest{};
 std::uint64_t expired{},rejected{},local_pid{},peer_pid{},local_birth{},peer_birth{},error{},flags{};
};
class TransportDiagnostics {
 struct Notice {
  bool retirement{},service{};Retirement reason{};TransportProgress progress{};
  Family family{};ServiceFailure failure{};std::uint64_t service_generation{};std::array<char,160> detail{};
  NativeCall call{};std::uint64_t ordinal{},begin{},end{};std::uint32_t error{};
 };
 DiagnosticSink sink_{};
 std::uint64_t run_{},sequence_{},call_sequence_{};
 DiagnosticSnapshot<20> progress_;
 DiagnosticSnapshot<4> call_;
 DiagnosticRing<Notice,64> notices_;
 DiagnosticRing<psx::diagnostics::Event,256> output_;
 std::atomic<std::uint64_t> lost_{};
 std::atomic<NativeCall> fixture_call_{NativeCall::none};
 std::atomic<std::uint32_t> fixture_delay_{};
 void emit(std::string_view,std::array<std::uint64_t,6>,std::uint64_t,
           const TransportProgress&,psx::diagnostics::Severity=psx::diagnostics::Severity::info,bool retain=true) noexcept;
public:
 void configure(DiagnosticSink,std::uint64_t run) noexcept;
 bool enabled() const noexcept{return sink_.submit!=nullptr;}
 void progress(const TransportProgress&) noexcept;
 void retire(Retirement,const TransportProgress&,std::uint64_t now,std::string_view detail={}) noexcept;
 void service_failure(Family,ServiceFailure,std::uint64_t,const TransportProgress&,std::uint64_t) noexcept;
 std::uint64_t begin(NativeCall,std::uint64_t&) noexcept;
 void end(NativeCall,std::uint64_t,std::uint64_t,std::uint32_t) noexcept;
 void observe(HANDLE stop) noexcept;
 bool pop(psx::diagnostics::Event& event) noexcept{return output_.pop(event);}
 std::uint64_t lost()const noexcept{return lost_.load(std::memory_order_relaxed);}
 void fixture_delay(NativeCall call,std::uint32_t ms) noexcept {
  fixture_delay_.store(ms,std::memory_order_relaxed);fixture_call_.store(call,std::memory_order_release);
 }
};
class NativeCallScope {
 TransportDiagnostics* diagnostics_{};NativeCall call_{};
 std::uint64_t ordinal_{},begin_{};
public:
 NativeCallScope(TransportDiagnostics* diagnostics,NativeCall call) noexcept:diagnostics_(diagnostics),call_(call) {
  if(diagnostics_&&diagnostics_->enabled()){const auto error=GetLastError();ordinal_=diagnostics_->begin(call_,begin_);SetLastError(error);}
 }
 ~NativeCallScope(){
  if(ordinal_){const auto error=GetLastError();diagnostics_->end(call_,ordinal_,begin_,error);SetLastError(error);}
 }
};
template<class Function,NativeCall Id> struct NativeApi {
 Function address{};TransportDiagnostics* diagnostics{};
 template<class... Args> decltype(auto) operator()(Args&&... args)const {
  NativeCallScope call(diagnostics,Id);return address(static_cast<Args&&>(args)...);
 }
};
}
