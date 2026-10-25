#include "observation_internal.hpp"
namespace coordinator {
namespace a=psx::audio;namespace b=psx::bridge;namespace e=psx::exporter;
void ObservationController::Impl::audio_record(const AudioEvidence& value){
    if(audio_evidence_size==audio_evidence.size()){++audio_evidence_lost;return;}
    audio_evidence[(audio_evidence_head+audio_evidence_size)%audio_evidence.size()]=value;++audio_evidence_size;
}
bool ObservationController::Impl::apply_audio(const AudioInput& input){AudioEvidence r;r.kind=1;r.time=input.now;r.input=input;audio_record(r);const auto overflows=audio.view().overflows;const auto accepted=audio.apply(input);if(audio.view().overflows!=overflows)capture.stop(CaptureStopReason::audio_transition);view.audio=audio.view();SetEvent(published.get());return accepted;}
void ObservationController::Impl::capture_audio_evidence(){
    if(audio_evidence_lost!=audio_evidence_lost_seen){capture.stop(CaptureStopReason::audio_evidence);audio_evidence_lost_seen=audio_evidence_lost;AudioInput reset;reset.kind=AudioInputKind::overflow;reset.now=qpc_now();audio.apply(reset);view.audio=audio.view();}
    while(audio_evidence_size){const auto& r=audio_evidence[audio_evidence_head];if(r.kind==1)capture.audio_input(r.input);else if(r.kind==2)capture.audio_frame(r.frame,r.time);else capture.audio_lease(r.lease,r.time);audio_evidence_head=(audio_evidence_head+1)%audio_evidence.size();--audio_evidence_size;}
}
bool ObservationController::Impl::audio_frame(b::Frame& frame,std::int64_t now){if(!audio.next_frame(frame,now))return false;AudioEvidence r;r.kind=2;r.time=now;r.frame=frame;audio_record(r);return true;}
void ObservationController::Impl::audio_tick(std::int64_t now){
    AudioInput tick;tick.kind=AudioInputKind::tick;tick.now=now;apply_audio(tick);a::Lease lease;
    if(audio.next_lease(lease)){AudioEvidence r;r.kind=3;r.time=now;r.lease=lease;audio_record(r);
        if(!exporter_service.lease(lease)){capture.stop(CaptureStopReason::audio_lease);AudioInput failed;failed.kind=AudioInputKind::overflow;failed.now=now;apply_audio(failed);}}
    view.audio=audio.view();
}
void ObservationController::Impl::audio_exporter(const ExporterEvent& event){
    AudioInput in;in.now=event.received_qpc;
    if(event.kind==ExporterEventKind::association||event.kind==ExporterEventKind::unavailable){in.kind=AudioInputKind::lost;apply_audio(in);return;}
    e::Header h;if(event.frame.size<e::header_size||!e::read_header(event.frame.bytes.data(),e::header_size,h)||event.frame.size!=e::header_size+h.payload_size)return;
    const auto* p=event.frame.bytes.data()+e::header_size;
    if(event.kind==ExporterEventKind::hello){e::Hello hello;if(!e::decode(p,h.payload_size,hello))return;in.kind=AudioInputKind::hello;in.x=hello.producer.high;in.y=hello.producer.low;in.z=h.version==2?hello.audio_groups:0;in.w=h.version==2?hello.suppression_groups:0;apply_audio(in);}
    else if(event.kind==ExporterEventKind::audio_snapshot){in.kind=AudioInputKind::snapshot;if(e::decode(p,h.payload_size,in.sample))apply_audio(in);else {in.kind=AudioInputKind::overflow;apply_audio(in);}}
    else if(event.kind==ExporterEventKind::audio_ack){in.kind=AudioInputKind::ack;if(e::decode(p,h.payload_size,in.ack))apply_audio(in);else {in.kind=AudioInputKind::overflow;apply_audio(in);}}
    else if(event.kind==ExporterEventKind::health&&h.version==2&&h.payload_size>=92){e::Health health;if(e::decode(p,h.payload_size,health)){in.kind=AudioInputKind::health;in.x=health.audio_enqueued;in.y=health.audio_sent;in.z=health.audio_dropped;in.w=std::uint64_t(health.audio_groups)|(std::uint64_t(health.suppression_groups)<<32);apply_audio(in);}}
}
void ObservationController::mute_aircraft_audio(bool muted){std::lock_guard<std::mutex> lock(impl_->mutex);AudioInput in;in.kind=AudioInputKind::mute;in.now=qpc_now();in.x=muted?1:0;impl_->apply_audio(in);SetEvent(impl_->acquisition_ready.get());}
}
