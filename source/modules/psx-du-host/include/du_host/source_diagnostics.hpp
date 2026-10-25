#pragma once
#include <psx/display/transport.hpp>
#include <psx/diagnostics/types.hpp>
namespace du_host {
// One host worker owns this cursor. On queue loss, retry the latest complete
// snapshot next pass. No diagnostic backlog or transport backpressure.
struct SourceReport {
    uint64_t epoch{},revision{},created{},generation{};uint32_t pid{};
    std::array<uint64_t,17> previous{};
    template<class Emit> bool publish(unsigned unit,const psx::du::transport::LocalView& v,Emit emit) {
        const auto& s=v.counters;
        if(!s.valid || (epoch==v.epoch&&revision==s.revision&&pid==v.origin.pid&&created==v.origin.created&&generation==s.generation))return true;
        using namespace psx::diagnostics;
        bool warning=false;const bool same_source=epoch==v.epoch&&pid==v.origin.pid&&created==v.origin.created;
        for(auto i:{2u,3u,4u,14u})if(s.values[i]>(same_source?previous[i]:0))warning=true;
        Event e;e.code=Code::metrics;e.severity=warning?Severity::warning:Severity::info;
        e.context.connection_id=v.epoch;e.context.session_epoch=s.generation;
        text(e.text,"DU recorder identity: unit,revision,PID,creation,lease,changeQPC");
        e.values={unit,s.revision,v.origin.pid,v.origin.created,s.generation,s.changed_qpc};
        bool complete=emit(e);
        for(unsigned first=0;first<17;first+=4){
            e.values={unit,s.revision,0,0,0,0};
            for(unsigned n=0;n<4&&first+n<17;++n)e.values[n+2]=s.values[first+n];
            constexpr const char* labels[]{"DU recorder 0..3: accepted,dropped,invalid_candidates,buffer_retries",
              "DU recorder 4..7: observer_failures,worker_transport_lost,paints,queue_depth",
              "DU recorder 8..11: native_queue_retries,native_transport_lost,gc_collections,gc_ms",
              "DU recorder 12..15: source_allocated_bytes,worker_allocated_bytes,abandoned_candidates,ordinary_primitives",
              "DU recorder 16: recorded_primitives"};
            text(e.text,labels[first/4]);complete=emit(e)&&complete;
        }
        if(complete){epoch=v.epoch;revision=s.revision;pid=v.origin.pid;created=v.origin.created;generation=s.generation;previous=s.values;}
        return complete;
    }
};
}
