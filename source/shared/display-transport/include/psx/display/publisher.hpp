#pragma once
#include "transport.hpp"
#include <psx/norm/du.hpp>
#include <psx/display/diagnostics.hpp>
namespace psx::du::transport {
struct HostOptions { std::string ethernet; uint32_t pid{};uint64_t created{};unsigned mask{63};bool enabled{true};bool operator==(const HostOptions&)const=default; };
struct BindingView { psx::norm::Stats stats;std::array<double,6> rate{},grtt{};psx::norm::NativeSnapshot native; };
struct HostView { std::array<LocalView,6> units;uint64_t published{},replaced{},expired{},rejected{},owned_peak{},prepared{},prepare_max_qpc{},diagnostic_lost{};size_t payload_bytes{},payload_peak{};bool ethernet{},loopback{},diagnostics_failed{};std::array<BindingView,2> bindings;std::string error; };
class HostPublisher {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 HostPublisher();~HostPublisher();void configure(HostOptions);void reconnect();void stop();HostView view()const;
 void observe(std::shared_ptr<Trace>); // Optional capture; fixed nine producers.
 void request_stop() noexcept; // Independent source-stop signal; stop() joins owners.
 // Optional synchronous owner-thread reporting. Attach before enabling; the
 // context outlives stop, and production sinks must never block or throw.
 void events(psx::norm::DiagnosticSink,void*);
};
size_t owned_bytes();size_t owned_peak();
}
