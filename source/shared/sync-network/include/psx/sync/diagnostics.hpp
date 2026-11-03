#pragma once
#include <psx/diagnostics/types.hpp>
namespace psx::sync {
// These numeric IDs describe the dynamic API boundary, not wire messages.
#define PSX_SYNC_NATIVE_CALLS(X) \
 X(CreateInstance) X(DestroyInstance) X(CreateSession) X(DestroySession) \
 X(StartReceiver) X(StopReceiver) X(StartSender) X(StopSender) \
 X(SetTxOnly) X(SetTxPort) X(SetRxPortReuse) X(SetDefaultUnicastNack) \
 X(SetDefaultSyncPolicy) X(SetRxCacheLimit) X(SetTxCacheBounds) X(SetTxRate) \
 X(GetTxRate) X(SetCongestionControl) X(SetGrttEstimate) X(SetTxLoss) X(SetRxLoss) \
 X(DataEnqueue) X(ObjectCancel) X(GetNextEvent) X(ObjectGetSize) X(ObjectGetType) \
 X(DataAccessData) X(DataDetachData) X(NodeGetAddress) X(SendCommand) \
 X(CancelCommand) X(NodeGetCommand) X(GetDescriptor) X(SetAllocationFunctions)
enum class NativeCall : std::uint16_t {
 none=0,
#define PSX_SYNC_CALL_ID(name) name,
 PSX_SYNC_NATIVE_CALLS(PSX_SYNC_CALL_ID)
#undef PSX_SYNC_CALL_ID
 count
};
enum class Retirement : std::uint16_t {
 shutdown=1, clock_expired, progress_expired, peer_identity, peer_boot,
 peer_connection, peer_replaced, peer_detach, clock_discontinuity,
 handshake_expired, native_exception
};
enum class ServiceFailure : std::uint16_t {ordered_expired=1,deferred_capacity,admission_capacity};
const char* native_call_name(NativeCall) noexcept;
const char* retirement_name(Retirement) noexcept;
// One dedicated observer owns submissions. Context must outlive Endpoint::stop.
// Its callback must not take endpoint/application locks or wait on file/network I/O.
struct DiagnosticSink {
 void* context{};
 void (*submit)(void*,const psx::diagnostics::Event&) noexcept{};
 psx::diagnostics::Context origin{};
};
}
