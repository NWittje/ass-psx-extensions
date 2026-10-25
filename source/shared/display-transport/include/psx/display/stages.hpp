#pragma once
#include <WinSock2.h>
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
namespace psx::display::diagnostics {

// Opaque to NORM: callers supply this token; no DU/wire parsing in this library.
struct Correlation { std::array<uint8_t,16> session{}; uint64_t generation{},publication{},frame{}; };
enum class Stage:uint16_t { assembly=1,prepared,replaced,expired,enqueue_enter,enqueue_return,sent,purged,cache_busy,receive_complete,validated,decode_enter,decoded,render_return,native_error,heartbeat,rate,grtt,destroyed,validation_enter,source_complete,payload_digest,invocation_enter,invocation_return,paint_commit,worker_dequeue,digest_enter,digest_return,png_enter,png_return,slot_release,jni_first,jni_last,jni_rejected,jni_retry,pipe_first_write,pipe_last_write,host_first_chunk,host_validation,raw_begin,raw_commit,ui_adopt,render_enter,unchanged,socket_send_enter,socket_send_return,socket_partial,socket_complete,acknowledged,reconnected,socket_first_byte,cancelled,budget_rejected,source_generation,source_paint,connection_identity,recorder_commit,discarded,gc_count,gc_millis,source_allocated,worker_allocated,connection_peer,source_identity };
struct DiagnosticRecord { Correlation token; uint64_t stamp{},value{},source_end{}; uint32_t thread{},flags{}; uint16_t unit{},stage{}; uint32_t reserved{}; };
static_assert(sizeof(DiagnosticRecord)==80 && std::is_trivially_copyable_v<DiagnosticRecord>);
inline uint64_t diagnostic_qpc() noexcept { LARGE_INTEGER v{};QueryPerformanceCounter(&v);return v.QuadPart; }
// Exactly one producer and one consumer; no producer allocation, locks or I/O.
struct DiagnosticQueue {
 std::array<DiagnosticRecord,4096> records{};
 alignas(64) std::atomic<uint64_t> head{},tail{},lost{};
 void push(DiagnosticRecord r) noexcept {
  auto h=head.load(std::memory_order_relaxed);
  if(h-tail.load(std::memory_order_acquire)==records.size()){lost.fetch_add(1,std::memory_order_relaxed);return;}
  records[h%records.size()]=r;head.store(h+1,std::memory_order_release);
 }
 bool pop(DiagnosticRecord&r) noexcept {
  auto t=tail.load(std::memory_order_relaxed);if(t==head.load(std::memory_order_acquire))return false;
  r=records[t%records.size()];tail.store(t+1,std::memory_order_release);return true;
 }
};
using DiagnosticSink=void(*)(void*,const DiagnosticRecord&) noexcept;
enum class NativeCall:uint32_t { idle,configuration,descriptor,get_event,enqueue,detach,cancel,rate,grtt,stop_instance,destroy_session,destroy_instance,
 library_load_version,create_instance,allocation_functions,create_session,tx_rate_bounds,rx_socket_buffer,node_address,object_size,object_type,
 SetRxPortReuse,SetMulticastInterface,SetTTL,SetMulticastLoopback,SetTxOnly,SetTxPort,SetCongestionControl,SetTxRate,SetGrttEstimate,SetTxCacheBounds,StartSender,SetTxLoss,SetDefaultNackingMode,SetSilentReceiver,SetDefaultSyncPolicy,SetRxCacheLimit,StartReceiver,SetRxLoss,flow_control,access_data,tcp_connect,tcp_accept,tcp_recv,tcp_send,tcp_completion,tcp_cancel,tcp_close };
struct NativeSnapshot { uint64_t entered{},completed{};uint32_t call{},thread{},unit{}; };
// Continuously readable by a supervisor; it never calls into the native engine.
struct NativeMonitor {
 std::atomic<uint64_t> entered{},completed{};std::atomic<uint32_t> call{},thread{},unit{};
 NativeSnapshot snapshot()const noexcept { return {entered.load(),completed.load(),call.load(),thread.load(),unit.load()}; }
};
}
