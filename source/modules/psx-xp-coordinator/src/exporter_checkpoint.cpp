#include "coordinator/exporter.hpp"
#include <stdexcept>
namespace coordinator {
namespace b=psx::bridge;namespace e=psx::exporter;namespace m=psx::motion;
namespace {
struct Write {
    std::vector<std::uint8_t> data;
    void u(std::uint64_t v,std::size_t n=8){for(std::size_t i=0;i<n;++i)data.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
    template<class F> void frame(const F& f){u(f.size,2);data.insert(data.end(),f.bytes.begin(),f.bytes.begin()+f.size);}
};
struct Read {
    const std::uint8_t* p;std::size_t size,pos{};
    std::uint64_t u(std::size_t n=8){if(n>size-pos)throw std::runtime_error("exporter checkpoint truncated");std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(p[pos++])<<(8*i);return v;}
    template<class T> void exporter(T& v,e::Kind kind){const auto n=static_cast<std::size_t>(u(2));if(!n)return;e::Header h;
        if(n>size-pos||n<e::header_size||!e::read_header(p+pos,e::header_size,h)||h.kind!=kind||n!=e::header_size+h.payload_size||!e::decode(p+pos+e::header_size,h.payload_size,v))throw std::runtime_error("exporter checkpoint frame");pos+=n;}
    template<class T> bool bridge(T& v,b::Kind kind){const auto n=static_cast<std::size_t>(u(2));if(!n)return false;b::Header h;
        if(n>size-pos||n<b::header_size||!b::read_capture_header(p+pos,b::header_size,h)||h.kind!=kind||n!=b::header_size+h.payload_size||!b::decode(p+pos+b::header_size,h.payload_size,v))throw std::runtime_error("exporter checkpoint bridge frame");pos+=n;return true;}
};
}
std::vector<std::uint8_t> exporter_checkpoint(const ExporterModel& model,std::uint16_t bridge_version){
    Write w;const auto& s=model.state();w.u(1,1);w.u(s.process.pid,4);w.u(s.process.creation_filetime);w.u((s.peer?1u:0u)|(s.ready?2u:0u)|(s.boundary_pending?4u:0u),1);w.u(s.error,4);
    const auto legacy=[&](e::Frame& f,bool hello){if(s.protocol_version==1){f.size=e::header_size+(hello?104:60);const auto n=f.size-e::header_size;for(unsigned i=0;i<4;++i)f.bytes[8+i]=static_cast<std::uint8_t>(n>>(i*8));f.bytes[4]=1;f.bytes[5]=0;}};
    e::Frame f;if(e::encode(s.hello,0,f)){legacy(f,true);w.frame(f);}else w.u(0,2);e::encode(s.health,0,f);legacy(f,false);w.frame(f);
    for(auto n:{s.raw,s.invalid,s.sequence_gaps,s.frame_gaps,s.handoff_dropped,s.boundaries,s.last_sequence,s.last_frame})w.u(n);
    for(auto n:{s.stable_since,s.last_valid_qpc,s.last_sample_end_qpc,s.last_sampling_width})w.u(static_cast<std::uint64_t>(n));
    w.u(static_cast<unsigned>(s.preference),1);w.u(static_cast<unsigned>(s.settings.mode),1);w.u(s.settings.delay_ms,2);
    b::Frame frame;if(!b::encode(s.selection,0,frame))throw std::logic_error("exporter selection checkpoint");frame.bytes[4]=static_cast<std::uint8_t>(bridge_version);frame.bytes[5]=0;w.frame(frame);
    const auto& c=model.history().counters();for(auto n:{c.accepted,c.evicted,c.equal_time,c.rejected,c.resets})w.u(n);
    const auto& history=model.history().window();w.u(history.revision);
    b::CompletePoseWindow window;window.connection_id=1;window.selection_epoch=s.selection.selection_epoch;window.samples=history;
    window.published_qpc=history.count?history.samples[history.count-1].received_qpc:0;
    if(b::encode(window,0,frame)){frame.bytes[4]=static_cast<std::uint8_t>(bridge_version);frame.bytes[5]=0;w.frame(frame);}else w.u(0,2);
    return std::move(w.data);
}
bool restore_exporter_checkpoint(ExporterModel& model,const std::uint8_t* data,std::size_t size){
    try{Read r{data,size};if(r.u(1)!=1)return false;ExporterState s;s.process.pid=static_cast<std::uint32_t>(r.u(4));s.process.creation_filetime=r.u();const auto flags=r.u(1);if(flags>7)return false;s.peer=(flags&1)!=0;s.ready=(flags&2)!=0;s.boundary_pending=(flags&4)!=0;s.error=static_cast<std::uint32_t>(r.u(4));
        const auto hello_position=r.pos; r.exporter(s.hello,e::Kind::hello);if(r.pos>hello_position+2)s.protocol_version=data[hello_position+2+4];
        const auto health_position=r.pos;r.exporter(s.health,e::Kind::health);if(r.pos>health_position+2)s.protocol_version=data[health_position+2+4];
        std::uint64_t* counters[]={&s.raw,&s.invalid,&s.sequence_gaps,&s.frame_gaps,&s.handoff_dropped,&s.boundaries,&s.last_sequence,&s.last_frame};for(auto* n:counters)*n=r.u();
        std::int64_t* times[]={&s.stable_since,&s.last_valid_qpc,&s.last_sample_end_qpc,&s.last_sampling_width};for(auto* n:times){const auto v=r.u();if(v>INT64_MAX)return false;*n=static_cast<std::int64_t>(v);}
        s.preference=static_cast<m::SourcePreference>(r.u(1));s.settings.mode=static_cast<m::ReconstructionMode>(r.u(1));s.settings.delay_ms=static_cast<std::uint16_t>(r.u(2));if(!r.bridge(s.selection,b::Kind::source_selection))return false;
        m::CompleteHistoryCounters c;c.accepted=r.u();c.evicted=r.u();c.equal_time=r.u();c.rejected=r.u();c.resets=r.u();const auto revision=r.u();b::CompletePoseWindow window;
        if(!r.bridge(window,b::Kind::complete_pose_window))window.samples.revision=revision;else if(window.samples.revision!=revision)return false;
        return r.pos==size&&model.restore(s,window.samples,c);
    }catch(...){return false;}
}
}
