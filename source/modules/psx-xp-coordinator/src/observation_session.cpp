#include "coordinator/observation_session.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace coordinator {
namespace b=psx::bridge;
namespace a=psx::aircraft;
namespace m=psx::motion;
namespace {
bool natural(std::string_view v) {
    std::uint32_t n{}; const auto r=std::from_chars(v.data(),v.data()+v.size(),n);
    return !v.empty() && r.ec==std::errc{} && r.ptr==v.data()+v.size();
}
a::SourceRecord source_for(SourceStream stream,std::string_view line) noexcept {
    if(stream==SourceStream::boost)return a::SourceRecord::boost;
    const auto key=line.substr(0,line.substr(0,6).find('='));
    struct Key{std::string_view text;a::SourceRecord source;};
    constexpr Key keys[]={{"Qi1",a::SourceRecord::qi1},{"Qi128",a::SourceRecord::qi128},{"Qi129",a::SourceRecord::qi129},{"Qi257",a::SourceRecord::qi257},{"Qs121",a::SourceRecord::qs121},{"Qs481",a::SourceRecord::qs481},{"Qs482",a::SourceRecord::qs482},{"Qs483",a::SourceRecord::qs483},{"Qs424",a::SourceRecord::qs424},{"Qs425",a::SourceRecord::qs425},{"Qs436",a::SourceRecord::qs436},{"Qs479",a::SourceRecord::qs479},{"Qs480",a::SourceRecord::qs480},{"Qs443",a::SourceRecord::qs443},{"Qi156",a::SourceRecord::qi156},{"Qi157",a::SourceRecord::qi157},{"Qi158",a::SourceRecord::qi158},{"Qi159",a::SourceRecord::qi159}};
    for(const auto& k:keys)if(key==k.text)return k.source;return a::SourceRecord::unknown;
}
bool initializer(std::string_view text,unsigned& flag) noexcept {
    if(text.substr(0,6)!="Qs122=")return false;text.remove_prefix(6);
    if(text.size()>200)return false;
    for(unsigned i=0;i<11;++i){const auto end=text.find(';');const auto field=text.substr(0,end);if(field.empty() || (i==10)!=(end==std::string_view::npos))return false;
        if(i==0){if(field.size()!=1 || field[0]<'0' || field[0]>'2')return false;flag=static_cast<unsigned>(field[0]-'0');}
        else if(i==8 || i==9){double value{};const auto parsed=std::from_chars(field.data(),field.data()+field.size(),value);if(parsed.ec!=std::errc{} || parsed.ptr!=field.data()+field.size() || !std::isfinite(value) || std::abs(value)>(i==8?m::pi/2:m::pi))return false;}
        else {std::int64_t value{};const auto parsed=std::from_chars(field.data(),field.data()+field.size(),value);if(parsed.ec!=std::errc{} || parsed.ptr!=field.data()+field.size())return false;}
        if(end!=std::string_view::npos)text.remove_prefix(end+1);
    }return true;
}
}
PauseSourceState pause_source_state(const b::LatestState& latest) noexcept {
    PauseSourceState state;const auto slot=static_cast<std::size_t>(a::SourceRecord::qi129);
    if(!latest.available()[slot])return state;
    const auto& source=latest.records()[slot].observation;double paused{},frozen{};
    if(!m::value(source,a::PropertyId::paused_all,paused) || !m::value(source,a::PropertyId::motion_frozen,frozen))return state;
    state.available=true;state.paused_all=paused!=0;state.motion_frozen=frozen!=0;
    state.raw_value=static_cast<std::uint8_t>((state.paused_all?1:0)|(state.motion_frozen?2:0));state.stamp=source.stamp;return state;
}
bool pause_source_changed(const PauseSourceState& before,const PauseSourceState& after) noexcept {
    return before.available!=after.available || (after.available &&
        (before.stamp.session_epoch!=after.stamp.session_epoch || before.raw_value!=after.raw_value));
}
ObservationSession::ObservationSession(std::uint64_t frequency,bool motion):motion_enabled_(motion) {detector_.frequency=frequency;latest_.reset(1);state_.connection_id=1;state_.revision=1;latest_.apply(state_);}
void ObservationSession::publish(std::int64_t qpc) {
    if (state_.revision==(std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("session revision");
    ++state_.revision; state_.published_qpc=qpc;
    if (!latest_.apply(state_)) throw std::logic_error("session transition");
    if(!(state_.flags&b::main_connected)) {
        if(history_.window().domain.session_epoch)history_.clear();
    } else history_.synchronize(b::history_domain(state_));
    for(std::size_t i=0;i<raw_.size();++i) if(!latest_.available()[i]) raw_[i]={};
}
void ObservationSession::new_session(m::Boundary reason) {
    if(state_.session_epoch==(std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("session epoch");
    ++state_.session_epoch; decoder_=PsxAircraftDecoder{};
    if(!decoder_.begin_session(state_.session_epoch)) throw std::logic_error("decoder epoch");
    state_.configuration_epoch=0; state_.family=a::EngineFamily::unknown; sequence_=0;
    state_.flags&=~(b::synchronized|b::boost_available);
    state_.motion_epoch=motion_enabled_?1:0;state_.motion_sequence_floor=motion_enabled_?1:0;state_.motion_boundary=motion_enabled_?reason:m::Boundary::none;detector_.available=false;
}
void ObservationSession::boundary(m::Boundary reason,std::uint64_t floor,std::int64_t time) {
    if(!motion_enabled_)return;if(!state_.motion_epoch || state_.motion_epoch==UINT64_MAX || !floor)throw std::overflow_error("motion epoch/floor");
    ++state_.motion_epoch;state_.motion_sequence_floor=floor;state_.motion_boundary=reason;state_.flags&=~b::boost_available;detector_.available=false;publish(time);
}
bool ObservationSession::restore_detector(const MotionDetectorCheckpoint& value) noexcept {
    if(value.received_qpc<0 || !m::valid(value.previous) || (value.available && (!value.frequency || !state_.session_epoch)))return false;
    detector_=value;return true;
}
void ObservationSession::detect_jump(const a::Observation& observation,LineResult& result,std::int64_t time) {
    if(!motion_enabled_ || !detector_.frequency)return;
    // Main load markers already own the discontinuity. Boost can arrive before,
    // during or after Main's dump; do not invalidate its newly supplied pitch
    // again from that interleaving. First Boost after load3 seeds history.
    if(!(state_.flags&b::synchronized)){detector_.available=false;return;}
    m::GeoPose position;
    if(!m::value(observation,a::PropertyId::latitude_deg,position.latitude_deg) || !m::value(observation,a::PropertyId::longitude_deg,position.longitude_deg) || !m::value(observation,a::PropertyId::altitude_msl_m,position.altitude_msl_m))return;
    const auto delta=observation.stamp.received_qpc-detector_.received_qpc;
    // Several complete records in one TCP receive share a timestamp. The fixed
    // minimum thresholds still detect a jump when that receive interval is zero.
    if(detector_.available && delta>=0 && static_cast<long double>(delta)*1000<=static_cast<long double>(detector_.frequency)*m::default_boost_timeout_ms) {
        result.jump_interval_s=static_cast<double>(delta)/static_cast<double>(detector_.frequency);double rate=1;
        const auto rate_slot=static_cast<std::size_t>(a::SourceRecord::qi128);if(latest_.available()[rate_slot])m::value(latest_.records()[rate_slot].observation,a::PropertyId::simulation_rate_ratio,rate);
        result.jump_rate=(std::max)(1.0,rate);result.jump_horizontal_m=m::distance_m(detector_.previous,position);result.jump_vertical_m=std::abs(position.altitude_msl_m-detector_.previous.altitude_msl_m);
        if(result.jump_horizontal_m>(std::max)(1000.0,800*result.jump_interval_s*result.jump_rate) || result.jump_vertical_m>(std::max)(500.0,250*result.jump_interval_s*result.jump_rate)) {
            boundary(m::Boundary::pose_jump,observation.stamp.sequence,time);result.motion_boundary=true;
        }
    }
    detector_.previous=position;detector_.received_qpc=observation.stamp.received_qpc;detector_.available=true;
}
void ObservationSession::event(SessionEvent e,std::int64_t qpc) {
    switch(e) {
    case SessionEvent::wanted_on: state_.flags|=b::wanted; break;
    case SessionEvent::wanted_off: state_.flags&=~b::wanted; break;
    case SessionEvent::main_up:
        new_session(); bootstrap_=0; identified_=false; state_.psx_version={};
        state_.flags=(state_.flags|b::main_connected)&~b::version_qualified; break;
    case SessionEvent::main_down: state_.flags&=~(b::main_connected|b::synchronized|b::version_qualified|b::boost_available);detector_.available=false;break;
    case SessionEvent::boost_up:
        if(state_.boost_generation==(std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("boost generation");
        ++state_.boost_generation;state_.flags=(state_.flags|b::boost_connected)&~b::boost_available;detector_.available=false;break;
    case SessionEvent::boost_down: state_.flags&=~(b::boost_connected|b::boost_available);detector_.available=false;break;
    default: throw std::invalid_argument("session event");
    }
    publish(qpc);
}
bool ObservationSession::selected(SourceStream stream,std::string_view line) noexcept {
    if(stream==SourceStream::boost) return true;
    if(line=="exit" || line=="load1" || line=="load2" || line=="load3" || line.substr(0,3)=="id=" ||
       line.substr(0,8)=="version=" || line.substr(0,7)=="layout=" || line.substr(0,6)=="Qs122=") return true;
    return source_for(stream,line)!=a::SourceRecord::unknown;
}
LineResult ObservationSession::line(SourceStream stream,std::string_view text,std::int64_t receive,std::int64_t decoded,std::int64_t (*clock)() noexcept) {
    LineResult result; result.selected=selected(stream,text);result.decoded_qpc=decoded;result.rejection_reason=9;
    if(receive<0 || decoded<receive) {result.malformed=true;result.source=source_for(stream,text);result.rejection_reason=8;return result;}
    if(text=="exit") {result.remote_exit=true;return result;}
    if(stream==SourceStream::main) {
        if(text.substr(0,6)=="Qs122=") {
            if(!motion_enabled_){result.selected=false;return result;}unsigned flag{};
            if(!initializer(text,flag)){result.malformed=true;result.rejection_reason=11;return result;}
            if(flag && bootstrap_==3 && (state_.flags&b::synchronized)){if(sequence_==UINT64_MAX)throw std::overflow_error("initializer floor");boundary(m::Boundary::initializer,sequence_+1,decoded);result.motion_boundary=true;}
            return result;
        }
        if(text.substr(0,3)=="id=") { identified_=natural(text.substr(3)); result.malformed=!identified_; return result; }
        if(text.substr(0,7)=="layout=") { result.malformed=!natural(text.substr(7));return result; }
        if(text.substr(0,8)=="version=") {
            const auto version=text.substr(8);
            if(version.empty() || version.size()>=state_.psx_version.size()) {result.malformed=true;return result;}
            // Preserve the complete printable vendor label for diagnostics.
            // Research qualification is informational, never an operating gate.
            for(unsigned char c:version) if(c<32 || c>126) {result.malformed=true;return result;}
            state_.psx_version={};std::copy(version.begin(),version.end(),state_.psx_version.begin());
            state_.flags&=~b::version_qualified;
            if(version=="10.180") state_.flags|=b::version_qualified;
            publish(decoded);return result;
        }
        if(text=="load1") {
            if(bootstrap_==3){new_session(m::Boundary::situation_load);result.motion_boundary=motion_enabled_;}
            else if(bootstrap_!=0) {result.bootstrap_error=true;result.rejection_reason=10;new_session(m::Boundary::situation_load);result.motion_boundary=motion_enabled_;}
            bootstrap_=1;state_.flags&=~b::synchronized;publish(decoded);return result;
        }
        if(text=="load2") { if(bootstrap_!=1){result.bootstrap_error=true;result.rejection_reason=10;}else bootstrap_=2;return result; }
        if(text=="load3") {
            if(bootstrap_!=2 || !identified_) {result.bootstrap_error=true;result.rejection_reason=10;return result;}
            bootstrap_=3;state_.flags|=b::synchronized;
            publish(decoded);return result;
        }
    }
    if(!result.selected || !(state_.flags&b::main_connected) ||
       (stream==SourceStream::boost && !(state_.flags&b::boost_connected))) return result;
    a::Observation o;
    if(sequence_==(std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("source sequence");
    const auto status=stream==SourceStream::main ? decoder_.decode_main(text,++sequence_,receive,o) : decoder_.decode_boost(text,++sequence_,receive,o);
    if(clock) {decoded=clock();result.decoded_qpc=decoded;}
    if(status!=DecodeResult::ok) { result.malformed=status!=DecodeResult::unsupported;result.source=source_for(stream,text);result.rejection_reason=static_cast<std::uint8_t>(status)+1;return result; }
    result.source=o.source;result.rejection_reason=0;
    if(stream==SourceStream::boost)detect_jump(o,result,decoded);
    if(state_.configuration_epoch!=decoder_.configuration_epoch()) {
        state_.configuration_epoch=decoder_.configuration_epoch();state_.family=decoder_.family();publish(decoded);
    }
    if(stream==SourceStream::boost && !(state_.flags&b::boost_available)) {state_.flags|=b::boost_available;publish(decoded);}
    b::ObservationMessage message;message.connection_id=1;message.session_revision=state_.revision;
    message.boost_generation=stream==SourceStream::boost?state_.boost_generation:0;
    message.decoded_qpc=decoded;message.published_qpc=decoded;message.observation=o;
    // Every accepted pose record enters history before the replaceable cache,
    // including records sharing one recv timestamp. No invented source spacing.
    history_.synchronize(b::history_domain(state_));
    if(o.source==a::SourceRecord::boost || o.source==a::SourceRecord::qs121)history_.append(o);
    if(!latest_.apply(message)) throw std::logic_error("decoded observation rejected");
    auto& raw=raw_[static_cast<std::size_t>(o.source)];
    if(text.size()>=raw.text.size()) throw std::logic_error("selected raw bound");
    raw={};raw.size=static_cast<std::uint16_t>(text.size());std::copy(text.begin(),text.end(),raw.text.begin());
    return result;
}
bool ObservationSession::restore(const b::SessionStatus& s,std::uint64_t sequence,std::uint8_t bootstrap,bool identified) {
    if(s.connection_id!=1 || bootstrap>3 || (s.flags&b::synchronized && (bootstrap!=3 || !identified))) return false;
    b::Frame check;if(!b::encode(s,0,check)) return false;
    PsxAircraftDecoder decoder;
    if(s.session_epoch) {if(!decoder.restore_context(s.session_epoch,s.configuration_epoch,s.family,sequence)) return false;}
    else if(sequence || s.configuration_epoch || s.family!=a::EngineFamily::unknown) return false;
    b::LatestState latest;latest.reset(1);if(!latest.apply(s)) return false;
    decoder_=decoder;state_=s;latest_=latest;sequence_=sequence;bootstrap_=bootstrap;identified_=identified;raw_={};detector_.available=false;history_={};if(s.flags&b::main_connected)history_.synchronize(b::history_domain(s));return true;
}
bool ObservationSession::restore_history(const m::SampleWindow& window,const m::HistoryCounters& counters) noexcept {
    const auto domain=(state_.flags&b::main_connected)?b::history_domain(state_):m::HistoryDomain{};
    if(!m::same_motion_domain(window.domain,domain) || window.domain.configuration_epoch!=domain.configuration_epoch)return false;
    for(std::size_t i=0;i<window.boost_count && i<m::history_capacity;++i)if(window.boost[i].stamp.sequence>sequence_)return false;
    for(std::size_t i=0;i<window.pitch_count && i<m::history_capacity;++i)if(window.pitch[i].stamp.sequence>sequence_)return false;
    if(window.has_pitch_anchor && window.pitch_anchor.stamp.sequence>sequence_)return false;
    return history_.restore_capture_checkpoint(window,counters);
}
bool ObservationSession::seed(const b::ObservationMessage& source,std::string_view raw) {
    const auto& o=source.observation;
    if(o.stamp.sequence>sequence_ || raw.size()>=RawSource{}.text.size()) return false;
    PsxAircraftDecoder decoder;
    // Qi1 changes unknown->configured while decoding, so seed it one generation
    // earlier with a distinct family only when a preceding generation exists.
    auto config=o.stamp.configuration_epoch;auto family=o.family;
    if(o.source==a::SourceRecord::qi1) {
        if(!config) return false;
        --config;family=config?(family==a::EngineFamily::ge?a::EngineFamily::pw:a::EngineFamily::ge):a::EngineFamily::unknown;
    }
    if(!decoder.restore_context(o.stamp.session_epoch,config,family,o.stamp.sequence-1)) return false;
    a::Observation decoded;
    const auto result=o.source==a::SourceRecord::boost ? decoder.decode_boost(raw,o.stamp.sequence,o.stamp.received_qpc,decoded) : decoder.decode_main(raw,o.stamp.sequence,o.stamp.received_qpc,decoded);
    if(result!=DecodeResult::ok) return false;
    b::ObservationMessage expected=source;expected.observation=decoded;
    b::Frame lhs,rhs;if(!b::encode(expected,0,lhs) || !b::encode(source,0,rhs) || lhs.size!=rhs.size || std::memcmp(lhs.bytes.data(),rhs.bytes.data(),lhs.size)) return false;
    if(!latest_.apply(source)) return false;
    auto& r=raw_[static_cast<std::size_t>(o.source)];r={};r.size=static_cast<std::uint16_t>(raw.size());std::copy(raw.begin(),raw.end(),r.text.begin());return true;
}
}
