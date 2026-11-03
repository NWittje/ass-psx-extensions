#pragma once
#include "diagnostics.hpp"
#include <psx/diagnostics/journal.hpp>
namespace psx::sync {
// Index6 is reserved for this endpoint's observer, on both application sides.
// It never shares an acquisition, SDK, bridge, audio or persistence producer.
inline DiagnosticSink journal_sink(psx::diagnostics::DiagnosticsHub& hub,
                                  psx::diagnostics::Context origin={}) noexcept {
 return {&hub,[](void* context,const psx::diagnostics::Event& event) noexcept {
  static_cast<psx::diagnostics::DiagnosticsHub*>(context)->emit(6,event);
 },origin};
}
}
