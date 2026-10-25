#include <psx/exporter/protocol.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace psx::exporter {
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559&&sizeof(float)==4&&std::numeric_limits<float>::is_iec559,
              "Exporter requires IEEE binary64 and binary32");
namespace {
struct Writer {
    std::uint8_t* data;std::size_t size,pos{};bool ok{true};
    void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(8*i));}
    void real(double v) noexcept {std::uint64_t bits{};std::memcpy(&bits,&v,8);u(bits,8);}
    void real(float v) noexcept {std::uint32_t bits{};std::memcpy(&bits,&v,4);u(bits,4);}
};
struct Reader {
    const std::uint8_t* data;std::size_t size,pos{};bool ok{true};
    std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(8*i);return v;}
    std::int64_t signed64() noexcept {const auto bits=u(8);std::int64_t v{};std::memcpy(&v,&bits,8);return v;}
    double real() noexcept {const auto bits=u(8);double v{};std::memcpy(&v,&bits,8);return v;}
    float single() noexcept {const auto bits=static_cast<std::uint32_t>(u(4));float v{};std::memcpy(&v,&bits,4);return v;}
    bool end() const noexcept {return ok&&pos==size;}
};
bool valid(const Hello& v) noexcept {
    if(!motion::valid(v.producer)||!v.pid||!v.process_creation_filetime||!v.qpc_frequency||v.qpc_frequency>1000000000000ULL||v.schema_revision!=motion::exporter_schema_revision)return false;
    if((v.audio_groups|v.suppression_groups)&~audio::all_groups)return false;
    bool ended=false;for(const char c:v.build){if(c==0)ended=true;else if(ended)return false;}return ended;
}
bool valid(const Subscribe& v) noexcept {return motion::valid(v.producer)&&v.coordinator_instance&&v.qpc_frequency&&v.qpc_frequency<=1000000000000ULL;}
// Java counters arrive on its diagnostic poll; native transport counters continue between polls.
bool valid(const Health& v) noexcept {return v.rejected<=v.attempted&&v.sent<=v.enqueued;}
void put(Writer& w,motion::InstanceId id) noexcept {w.u(id.high,8);w.u(id.low,8);}
void get(Reader& r,motion::InstanceId& id) noexcept {id.high=r.u(8);id.low=r.u(8);}
void put(Writer& w,const Hello& v) noexcept {
    put(w,v.producer);w.u(v.pid,4);w.u(v.process_creation_filetime,8);w.u(v.qpc_frequency,8);w.u(v.schema_revision,4);
    for(const char c:v.build)w.u(static_cast<unsigned char>(c),1);w.u(v.audio_groups,4);w.u(v.suppression_groups,4);
}
void get(Reader& r,Hello& v) noexcept {
    get(r,v.producer);v.pid=static_cast<std::uint32_t>(r.u(4));v.process_creation_filetime=r.u(8);v.qpc_frequency=r.u(8);v.schema_revision=static_cast<std::uint32_t>(r.u(4));
    for(auto& c:v.build)c=static_cast<char>(r.u(1));
    if(r.pos<r.size){v.audio_groups=static_cast<std::uint32_t>(r.u(4));v.suppression_groups=static_cast<std::uint32_t>(r.u(4));}
}
void put(Writer& w,const Subscribe& v) noexcept {put(w,v.producer);w.u(v.coordinator_instance,8);w.u(v.qpc_frequency,8);}
void get(Reader& r,Subscribe& v) noexcept {get(r,v.producer);v.coordinator_instance=r.u(8);v.qpc_frequency=r.u(8);}
void put(Writer& w,const Snapshot& v) noexcept {
    w.u(v.sequence,8);w.u(v.frame_sequence,8);w.u(v.pose_epoch,8);
    for(auto t:{v.sample_start_qpc,v.sample_end_qpc,v.scheduler_wall_ms})w.u(static_cast<std::uint64_t>(t),8);
    w.real(v.delta_ms);w.real(v.delta_acc_ms);w.real(v.simulation_rate);w.u(v.flags,4);
    w.real(v.latitude_rad);w.real(v.longitude_rad);w.real(v.heading_rad);
    for(auto x:{v.total_pitch_rad,v.bank_rad,v.altitude_ft,v.main_strut_ft,v.base_pitch_rad,v.nose_osc_rad,v.nose_compress_rad,
                v.cached_sin_pitch,v.stock_eye_altitude_ft,v.longitudinal_heading_rad,v.fpv_rad,v.slope_fade})w.real(x);
    w.real(v.stock_eye_latitude_rad);w.real(v.stock_eye_longitude_rad);
}
void get(Reader& r,Snapshot& v) noexcept {
    v.sequence=r.u(8);v.frame_sequence=r.u(8);v.pose_epoch=r.u(8);
    v.sample_start_qpc=r.signed64();v.sample_end_qpc=r.signed64();v.scheduler_wall_ms=r.signed64();
    v.delta_ms=r.real();v.delta_acc_ms=r.real();v.simulation_rate=r.real();v.flags=static_cast<std::uint32_t>(r.u(4));
    v.latitude_rad=r.real();v.longitude_rad=r.real();v.heading_rad=r.real();
    v.total_pitch_rad=r.single();v.bank_rad=r.single();v.altitude_ft=r.single();v.main_strut_ft=r.single();v.base_pitch_rad=r.single();
    v.nose_osc_rad=r.single();v.nose_compress_rad=r.single();v.cached_sin_pitch=r.single();v.stock_eye_altitude_ft=r.single();
    v.longitudinal_heading_rad=r.single();v.fpv_rad=r.single();v.slope_fade=r.single();
    v.stock_eye_latitude_rad=r.real();v.stock_eye_longitude_rad=r.real();
}
void put(Writer& w,const Health& v) noexcept {
    w.u(v.attempted,8);w.u(v.rejected,8);w.u(v.pose_epoch,8);w.u(v.flags,4);
    w.u(v.enqueued,8);w.u(v.sent,8);w.u(v.queue_dropped,8);w.u(v.transport_dropped,8);
    w.u(v.audio_enqueued,8);w.u(v.audio_sent,8);w.u(v.audio_dropped,8);w.u(v.audio_groups,4);w.u(v.suppression_groups,4);
}
void get(Reader& r,Health& v) noexcept {
    v.attempted=r.u(8);v.rejected=r.u(8);v.pose_epoch=r.u(8);v.flags=static_cast<std::uint32_t>(r.u(4));
    v.enqueued=r.u(8);v.sent=r.u(8);v.queue_dropped=r.u(8);v.transport_dropped=r.u(8);
    if(r.pos<r.size){v.audio_enqueued=r.u(8);v.audio_sent=r.u(8);v.audio_dropped=r.u(8);v.audio_groups=static_cast<std::uint32_t>(r.u(4));v.suppression_groups=static_cast<std::uint32_t>(r.u(4));}
}
template<class T> bool pack(const T& value,Kind kind,std::uint32_t sequence,Frame& out) noexcept {
    if(!valid(value))return false;
    Frame temp;Writer w{temp.bytes.data()+header_size,max_payload};put(w,value);if(!w.ok)return false;
    Writer h{temp.bytes.data(),header_size};h.u(0x45535850,4);h.u(protocol_version,2);h.u(static_cast<unsigned>(kind),2);h.u(w.pos,4);h.u(sequence,4);
    temp.size=header_size+w.pos;out=temp;return true;
}
template<class T> bool unpack(const std::uint8_t* data,std::size_t size,T& out) noexcept {
    if(size>max_payload)return false;T temp;Reader r{data,size};get(r,temp);if(!r.end()||!valid(temp))return false;out=temp;return true;
}
} // namespace
bool read_header(const std::uint8_t* data,std::size_t size,Header& out) noexcept {
    if(size!=header_size)return false;Reader r{data,size};if(r.u(4)!=0x45535850)return false;
    Header h;h.version=static_cast<std::uint16_t>(r.u(2));if(h.version!=1&&h.version!=protocol_version)return false;h.kind=static_cast<Kind>(r.u(2));h.payload_size=static_cast<std::uint32_t>(r.u(4));h.sequence=static_cast<std::uint32_t>(r.u(4));
    if(!r.end()||static_cast<unsigned>(h.kind)<1||static_cast<unsigned>(h.kind)>(h.version==1?4u:7u)||h.payload_size>max_payload)return false;out=h;return true;
}
bool encode(const Hello& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::hello,n,f);}
bool encode(const Subscribe& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::subscribe,n,f);}
bool encode(const Snapshot& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::snapshot,n,f);}
bool encode(const Health& v,std::uint32_t n,Frame& f) noexcept {return pack(v,Kind::health,n,f);}
bool decode(const std::uint8_t* d,std::size_t n,Hello& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,Subscribe& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,Snapshot& v) noexcept {return unpack(d,n,v);}
bool decode(const std::uint8_t* d,std::size_t n,Health& v) noexcept {return unpack(d,n,v);}

namespace {
template<class T> bool pack_audio(const T& value,Kind kind,std::uint32_t sequence,Frame& out) noexcept {
 Frame temp;std::size_t n{};if(!audio::encode_payload(value,temp.bytes.data()+header_size,max_payload,n))return false;
 Writer h{temp.bytes.data(),header_size};h.u(0x45535850,4);h.u(protocol_version,2);h.u(static_cast<unsigned>(kind),2);h.u(n,4);h.u(sequence,4);temp.size=header_size+n;out=temp;return true;
}
}
bool encode(const AudioSnapshot& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_snapshot,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioSnapshot& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode(const AudioLease& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_lease,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioLease& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode(const AudioAck& v,std::uint32_t s,Frame& f) noexcept {return pack_audio(v,Kind::audio_ack,s,f);}
bool decode(const std::uint8_t* p,std::size_t n,AudioAck& v) noexcept {return n<=max_payload&&audio::decode_payload(p,n,v);}
bool encode_legacy_subscribe(const Subscribe& v,std::uint32_t n,Frame& f) noexcept {if(!encode(v,n,f))return false;f.bytes[4]=1;return true;}
bool Framer::feed(const void* data,std::size_t size,FrameHandler fn,void* context) noexcept {
    if(failed_||(!data&&size)||!fn){failed_=true;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(data);
    while(size) {
        const auto n=(std::min)(size,needed_-used_);std::memcpy(buffer_.data()+used_,bytes,n);used_+=n;bytes+=n;size-=n;
        if(used_==needed_) {
            Header h;if(!read_header(buffer_.data(),header_size,h)){failed_=true;return false;}needed_=header_size+h.payload_size;
            if(used_==needed_) {
                try {if(!fn(context,h,buffer_.data()+header_size,h.payload_size)){failed_=true;return false;}}
                catch(...){failed_=true;return false;}
                used_=0;needed_=header_size;
            }
        }
    }
    return true;
}
} // namespace psx::exporter
