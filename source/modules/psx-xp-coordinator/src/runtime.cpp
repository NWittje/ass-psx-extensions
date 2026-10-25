#include "coordinator/runtime.hpp"
namespace coordinator {
Runtime::Runtime() : Runtime(psx::diagnostics::default_options(psx::diagnostics::Component::application)) {}
Runtime::Runtime(const psx::diagnostics::Options &options)
    : exporter(diagnostics), tcp(status, log), pipe(status, log), observation(log, &diagnostics) {
    if (!diagnostics.start(options))
        throw std::runtime_error("Diagnostics journal startup rejected");
    diagnostics.producer(0, psx::diagnostics::Component::application).emit(psx::diagnostics::Code::process_start);
    log.add(Level::info, "Coordinator started disconnected; observation Connect is explicit");
}
// Request every worker's stop before joining; keep the journal alive through
// worker drain so final transport and observation events retain their owner.
Runtime::~Runtime() {
    request_stop();
    join();
    diagnostics.producer(0, psx::diagnostics::Component::application).emit(psx::diagnostics::Code::process_stop);
    diagnostics.stop();
}
void Runtime::request_stop() noexcept {
    tcp.request_stop();
    pipe.request_stop();
    observation.disconnect();
    observation.stop_non_normal();
    exporter.cancel();
}
bool Runtime::stopped() const noexcept {
    return tcp.done() && pipe.done() && observation.done() && observation.non_normal_done() && exporter.done();
}
void Runtime::join() {
    tcp.join();
    pipe.join();
    observation.join();
    observation.join_non_normal();
    exporter.join();
}
bool Runtime::start_non_normal() {
    const bool started = observation.start_non_normal();
    if (!started)
        log.add(Level::warning, "Non-normal API unavailable; observation and synchronization remain independent");
    return started;
}
void Runtime::marker(std::string_view text) {
    psx::diagnostics::Event event;
    event.component = psx::diagnostics::Component::application;
    event.code = psx::diagnostics::Code::operator_marker;
    psx::diagnostics::text(event.text, text);
    diagnostics.emit_stamped(0, event);
    observation.record_diagnostic(event);
}
}
