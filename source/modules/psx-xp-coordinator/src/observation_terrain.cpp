#include "observation_internal.hpp"
namespace coordinator {
void ObservationController::Impl::drain_terrain(){
    std::lock_guard<std::mutex> lock(mutex);auto now=terrain_drain_clock();
    if(view.bridge==State::connected&&view.bridge_peer_process_id&&terrain_attachment!=view.bridge_attachments){terrain_service.plugin(terrain_process_identity(view.bridge_peer_process_id));terrain_attachment=view.bridge_attachments;}
    else if(view.bridge!=State::connected&&terrain_attachment){terrain_service.plugin({});terrain_attachment=0;terrain.following(false,now);}
    terrain.physical(exporter.physical(),now);
    const bool capturing=terrain_capture.status().active;terrain.capture(capturing,now);terrain_service.capture(capturing,terrain.capture_generation(),now);
    TerrainEvent event;for(std::size_t n=0;n<64&&terrain_service.pop(event);++n){
        psx::terrain::Header header;const bool detail=event.kind==TerrainEventKind::frame&&psx::terrain::read_header(event.frame.bytes.data(),psx::terrain::header_size,header)&&(header.kind==psx::terrain::Kind::probe_detail||header.kind==psx::terrain::Kind::support_detail||header.kind==psx::terrain::Kind::render_detail||header.kind==psx::terrain::Kind::constraint_detail||header.kind==psx::terrain::Kind::frame_detail);
        if(event.kind==TerrainEventKind::frame&&(!detail||terrain.capture_detail(event)))terrain_capture.record(1,event.role,event.frame.bytes.data(),event.frame.size,event.received_qpc);
        if(!terrain.event(event))terrain_service.fault(event.role,ERROR_INVALID_DATA);
    }
    // Probe completion/receipt can follow the entry clock while ingress is
    // drained. Forward that accepted plane using a later clock, preserving its
    // original source age and the strict100ms support admission limit.
    now=terrain_drain_clock();terrain.tick(now);
    if(terrain_service.dropped()!=terrain_dropped_seen){terrain_dropped_seen=terrain_service.dropped();terrain_capture.incomplete(ERROR_BUFFER_OVERFLOW,now);}
    for(const auto role:{psx::terrain::Role::plugin,psx::terrain::Role::exporter})for(std::size_t n=0;n<8;++n){psx::terrain::Frame frame;if(!terrain.next(role,frame,now))break;
        terrain_capture.record(2,role,frame.bytes.data(),frame.size,now);if(!terrain_service.send(role,frame)){terrain_service.fault(role,ERROR_BUFFER_OVERFLOW);break;}}
    view.terrain=terrain.view();
}
}
