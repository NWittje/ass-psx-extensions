#include "coordinator/diagnostics.hpp"
namespace coordinator {
namespace d=psx::diagnostics;
namespace b=psx::bridge;
d::Context diagnostic_context(const b::SessionStatus& s) noexcept {
    d::Context c;c.connection_id=s.connection_id;c.session_epoch=s.session_epoch;
    c.configuration_epoch=s.configuration_epoch;c.boost_generation=s.boost_generation;return c;
}
HealthReport HealthEvaluator::evaluate(const ObservationSnapshot& s,const d::JournalStatus& j,
    std::int64_t now,std::uint64_t frequency,std::uint32_t delayed_ms) noexcept {
    HealthReport h;h.source_ready=(s.latest.session().flags&(b::synchronized|b::boost_available))==(b::synchronized|b::boost_available) && s.latest.session().family!=psx::aircraft::EngineFamily::unknown;
    if(s.exporter.selection.source==psx::motion::PoseSource::exporter)h.source_ready=s.exporter.peer&&s.complete_window.count&&frequency&&now>=s.exporter.last_sample_end_qpc&&now-s.exporter.last_sample_end_qpc<static_cast<std::int64_t>(frequency/10);
    h.bridge_connected=s.bridge==State::connected;h.storage_degraded=j.degraded || j.persistence_error || j.lost || j.persistence_lost;
    h.report=d::freshness(s.plugin_available && h.bridge_connected,now,s.plugin_received_qpc,frequency,delayed_ms);
    const auto& p=s.plugin;
    h.callback=d::freshness(s.plugin_available && h.bridge_connected && (p.diagnostic_validity&b::callback_snapshot_valid)!=0,
        now,p.callback_qpc,frequency,delayed_ms);
    h.plugin_consuming=h.report==d::Freshness::current && h.callback==d::Freshness::current && (p.flags&b::observation_ready)!=0;
    h.counter_baseline_valid=s.plugin_available && (p.diagnostic_validity&b::counter_baseline_valid)!=0 && p.plugin_load_id && p.enable_id;
    if(h.counter_baseline_valid) {
        if(!baseline_ || load_!=p.plugin_load_id || enable_!=p.enable_id) {
            baseline_=true;load_=p.plugin_load_id;enable_=p.enable_id;
            dropped_=p.diagnostics_dropped;events_=p.events_lost;disk_=p.persistence_lost;
        }
        h.counter_delta_valid=p.diagnostics_dropped>=dropped_ && p.events_lost>=events_ && p.persistence_lost>=disk_;
        if(h.counter_delta_valid){h.diagnostic_dropped=p.diagnostics_dropped-dropped_;h.events_lost=p.events_lost-events_;h.persistence_lost=p.persistence_lost-disk_;}
    }
    return h;
}
void log_startup_failure(std::string_view message,d::ErrorDomain domain,std::uint32_t error,const d::Options* supplied) noexcept {
    try {d::DiagnosticsHub hub;auto options=supplied?*supplied:d::default_options(d::Component::application);
        if(hub.start(options)){hub.producer(0,d::Component::application).emit(d::Code::startup_failed,d::Severity::error,{},domain,error,message);hub.stop();}}
    catch(...) {} // Never mask the original handled startup failure.
}
}
