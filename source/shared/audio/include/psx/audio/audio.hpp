#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace psx::audio {
inline constexpr std::uint32_t revision=1;
inline constexpr std::size_t group_count=8, field_count=32, transition_capacity=64;
inline constexpr std::uint32_t all_groups=255;
inline constexpr std::uint32_t lease_ms=1000, renewal_ms=250;
enum class Group : std::uint8_t {engines,gear,flaps,apu,packs,hydraulic,fuel,airflow};
enum class Source : std::uint8_t {none,public_main,exporter};
enum class Family : std::uint8_t {unknown,ge,pw,rr};
enum class Phase : std::uint8_t {inactive,preparing,active,muted,held,unavailable};
enum class Reason : std::uint8_t {none,not_following,profile,assets,inputs,suppression,expired,source_lost,epoch,overflow,disabled,abi};
enum class Edge : std::uint8_t {rising,falling};
constexpr std::uint32_t bit(Group g) noexcept {return 1u<<static_cast<unsigned>(g);}
// Engine slots: engine*6 + N1,N2,N3,combustion,starter,reverser.
// Spools are raw PSX tenths-percent floats; reverser is [0,1].
// Gear: nose, left wing, right wing, left body, right body, ratios [0,1].
// Flaps: six LE and four TE raw source ext coordinates, then ten inTransit bools.
// APU: raw N1,N2 tenths-percent, running, starting. Packs: three fully-open valve bools, then three associated duct PSI.
// Hydraulic: four actual PSI. Fuel: up to 32 modeled actual-running bools, manifest ordering.
// Airflow: TAS m/s, OAT Celsius. No physical state is inferred from audio parameter names.
inline constexpr std::size_t engine_stride=6, n1=0,n2=1,n3=2,combustion=3,starter=4,reverser=5;
enum SnapshotFlag : std::uint32_t {coherent=1,held=2};
struct Snapshot {
    Group group{Group::engines}; Source source{Source::none}; Family family{Family::unknown};
    std::uint8_t variant{};
    std::uint64_t sequence{},lifecycle_epoch{},configuration_epoch{};
    std::int64_t sample_start_qpc{},sample_end_qpc{},received_qpc{};
    std::uint32_t valid_fields{},flags{};
    std::array<float,field_count> values{};
    bool has(std::size_t i) const noexcept {return i<field_count && (valid_fields&(1u<<i));}
};
struct State {
    std::uint64_t connection_id{},audio_epoch{},producer_high{},producer_low{};
    std::uint32_t available_groups{},suppression_capabilities{};
    std::array<Snapshot,group_count> groups{};
};
struct Transition {
    std::uint64_t connection_id{},audio_epoch{},sequence{},source_sequence{};
    std::int64_t sample_qpc{};
    Group group{Group::engines}; std::uint8_t field{}; Edge edge{Edge::rising};
};
// Same lease travels coordinator -> exporter -> coordinator -> plugin.
// Expiry uses QPC shared by processes on this Windows host. Mask zero releases.
struct Lease {
    std::uint64_t owner{},operation{},audio_epoch{};
    std::int64_t expires_qpc{};
    std::uint32_t groups{};
};
struct LeaseAck {Lease lease{};std::uint32_t accepted_groups{};Reason reason{Reason::none};};
struct Control {
    std::uint64_t connection_id{},audio_epoch{},plugin_generation{},aircraft_generation{};
    LeaseAck ownership{};
    bool follow{},muted{};
};
struct Status {
    std::uint64_t connection_id{},audio_epoch{},plugin_generation{},aircraft_generation{};
    std::int64_t generated_qpc{},lease_expires_qpc{},applied_sample_qpc{};
    std::uint32_t ready_groups{},active_groups{},held_groups{},missing_groups{};
    Phase phase{Phase::inactive};Reason reason{Reason::none};Family family{Family::unknown};bool muted{};
    std::uint64_t applied_sequence{},parameter_updates{},saturations{},dropped_transitions{},resets{};
};
inline bool valid(const Snapshot& s) noexcept {
    if(static_cast<unsigned>(s.group)>=group_count || static_cast<unsigned>(s.source)>2 || static_cast<unsigned>(s.family)>3 ||
       !s.sequence || !s.lifecycle_epoch || !s.configuration_epoch || s.sample_start_qpc<0 || s.sample_end_qpc<s.sample_start_qpc ||
       s.received_qpc<0 || (s.flags&~3u) || !s.valid_fields || s.source==Source::none) return false;
    for(std::size_t i=0;i<field_count;++i)if(!std::isfinite(s.values[i]))return false;
    return true;
}
inline bool valid(const State& s) noexcept {
    if(!s.connection_id || !s.audio_epoch || ((s.available_groups|s.suppression_capabilities)&~all_groups))return false;
    for(std::size_t i=0;i<group_count;++i)if(s.available_groups&(1u<<i))
        if(static_cast<std::size_t>(s.groups[i].group)!=i || !valid(s.groups[i]))return false;
    return true;
}
inline bool valid(const Transition& t) noexcept {return t.connection_id&&t.audio_epoch&&t.sequence&&t.source_sequence&&t.sample_qpc>=0&&static_cast<unsigned>(t.group)<group_count&&t.field<field_count&&static_cast<unsigned>(t.edge)<2;}
inline bool valid(const Lease& l) noexcept {return l.owner&&l.operation&&l.audio_epoch&&l.expires_qpc>=0&&!(l.groups&~all_groups)&&(!l.groups||l.expires_qpc>0);}
inline bool valid(const LeaseAck& a) noexcept {return valid(a.lease)&&!(a.accepted_groups&~a.lease.groups)&&static_cast<unsigned>(a.reason)<=static_cast<unsigned>(Reason::abi);}
inline bool valid(const Control& c) noexcept {return c.connection_id&&c.audio_epoch&&valid(c.ownership)&&c.ownership.lease.audio_epoch==c.audio_epoch;}
inline bool valid(const Status& s) noexcept {return s.connection_id&&s.audio_epoch&&s.generated_qpc>=0&&s.lease_expires_qpc>=0&&s.applied_sample_qpc>=0&&!((s.ready_groups|s.active_groups|s.held_groups|s.missing_groups)&~all_groups)&&static_cast<unsigned>(s.phase)<=5&&static_cast<unsigned>(s.reason)<=11&&static_cast<unsigned>(s.family)<=3;}
inline bool owns(const LeaseAck& a,std::int64_t now,std::uint64_t epoch,Group group) noexcept {return valid(a)&&a.reason==Reason::none&&a.lease.audio_epoch==epoch&&now<a.lease.expires_qpc&&(a.accepted_groups&bit(group));}
// Compact explicit LE payload codec shared by both envelopes; absent groups are omitted.
struct Writer {
    std::uint8_t* data;std::size_t size,pos{};bool ok{true};
    void u(std::uint64_t v,std::size_t n) noexcept {if(pos>size||n>size-pos){ok=false;return;}for(std::size_t i=0;i<n;++i)data[pos++]=static_cast<std::uint8_t>(v>>(i*8));}
    void f(float v) noexcept {std::uint32_t b{};std::memcpy(&b,&v,4);u(b,4);}
};
struct Reader {
    const std::uint8_t* data;std::size_t size,pos{};bool ok{true};
    std::uint64_t u(std::size_t n) noexcept {if(!data||pos>size||n>size-pos){ok=false;return 0;}std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(data[pos++])<<(i*8);return v;}
    std::int64_t q() noexcept {auto v=u(8);if(v>0x7fffffffffffffffULL)ok=false;return static_cast<std::int64_t>(v);}
    float f() noexcept {auto b=static_cast<std::uint32_t>(u(4));float v{};std::memcpy(&v,&b,4);return v;}
};
inline void put(Writer& w,const Snapshot& v) noexcept {
    w.u(static_cast<unsigned>(v.group),1);w.u(static_cast<unsigned>(v.source),1);w.u(static_cast<unsigned>(v.family),1);w.u(v.variant,1);
    for(auto x:{v.sequence,v.lifecycle_epoch,v.configuration_epoch})w.u(x,8);
    for(auto x:{v.sample_start_qpc,v.sample_end_qpc,v.received_qpc})w.u(static_cast<std::uint64_t>(x),8);
    w.u(v.valid_fields,4);w.u(v.flags,4);for(float x:v.values)w.f(x);
}
inline void get(Reader& r,Snapshot& v) noexcept {
    v.group=static_cast<Group>(r.u(1));v.source=static_cast<Source>(r.u(1));v.family=static_cast<Family>(r.u(1));v.variant=static_cast<std::uint8_t>(r.u(1));
    v.sequence=r.u(8);v.lifecycle_epoch=r.u(8);v.configuration_epoch=r.u(8);v.sample_start_qpc=r.q();v.sample_end_qpc=r.q();v.received_qpc=r.q();
    v.valid_fields=static_cast<std::uint32_t>(r.u(4));v.flags=static_cast<std::uint32_t>(r.u(4));for(auto& x:v.values)x=r.f();
}
inline void put(Writer& w,const State& v) noexcept {for(auto x:{v.connection_id,v.audio_epoch,v.producer_high,v.producer_low})w.u(x,8);w.u(v.available_groups,4);w.u(v.suppression_capabilities,4);for(std::size_t i=0;i<group_count;++i)if(v.available_groups&(1u<<i))put(w,v.groups[i]);}
inline void get(Reader& r,State& v) noexcept {v.connection_id=r.u(8);v.audio_epoch=r.u(8);v.producer_high=r.u(8);v.producer_low=r.u(8);v.available_groups=static_cast<std::uint32_t>(r.u(4));v.suppression_capabilities=static_cast<std::uint32_t>(r.u(4));for(std::size_t i=0;i<group_count;++i)if(v.available_groups&(1u<<i))get(r,v.groups[i]);}
inline void put(Writer& w,const Transition& v) noexcept {for(auto x:{v.connection_id,v.audio_epoch,v.sequence,v.source_sequence})w.u(x,8);w.u(static_cast<std::uint64_t>(v.sample_qpc),8);w.u(static_cast<unsigned>(v.group),1);w.u(v.field,1);w.u(static_cast<unsigned>(v.edge),1);}
inline void get(Reader& r,Transition& v) noexcept {v.connection_id=r.u(8);v.audio_epoch=r.u(8);v.sequence=r.u(8);v.source_sequence=r.u(8);v.sample_qpc=r.q();v.group=static_cast<Group>(r.u(1));v.field=static_cast<std::uint8_t>(r.u(1));v.edge=static_cast<Edge>(r.u(1));}
inline void put(Writer& w,const Lease& v) noexcept {for(auto x:{v.owner,v.operation,v.audio_epoch})w.u(x,8);w.u(static_cast<std::uint64_t>(v.expires_qpc),8);w.u(v.groups,4);}
inline void get(Reader& r,Lease& v) noexcept {v.owner=r.u(8);v.operation=r.u(8);v.audio_epoch=r.u(8);v.expires_qpc=r.q();v.groups=static_cast<std::uint32_t>(r.u(4));}
inline void put(Writer& w,const LeaseAck& v) noexcept {put(w,v.lease);w.u(v.accepted_groups,4);w.u(static_cast<unsigned>(v.reason),1);}
inline void get(Reader& r,LeaseAck& v) noexcept {get(r,v.lease);v.accepted_groups=static_cast<std::uint32_t>(r.u(4));v.reason=static_cast<Reason>(r.u(1));}
inline void put(Writer& w,const Control& v) noexcept {for(auto x:{v.connection_id,v.audio_epoch,v.plugin_generation,v.aircraft_generation})w.u(x,8);put(w,v.ownership);w.u(v.follow?1:0,1);w.u(v.muted?1:0,1);}
inline void get(Reader& r,Control& v) noexcept {v.connection_id=r.u(8);v.audio_epoch=r.u(8);v.plugin_generation=r.u(8);v.aircraft_generation=r.u(8);get(r,v.ownership);auto f=r.u(1),m=r.u(1);if(f>1||m>1)r.ok=false;v.follow=f!=0;v.muted=m!=0;}
inline void put(Writer& w,const Status& v) noexcept {
    for(auto x:{v.connection_id,v.audio_epoch,v.plugin_generation,v.aircraft_generation})w.u(x,8);
    for(auto x:{v.generated_qpc,v.lease_expires_qpc,v.applied_sample_qpc})w.u(static_cast<std::uint64_t>(x),8);
    for(auto x:{v.ready_groups,v.active_groups,v.held_groups,v.missing_groups})w.u(x,4);
    w.u(static_cast<unsigned>(v.phase),1);w.u(static_cast<unsigned>(v.reason),1);w.u(static_cast<unsigned>(v.family),1);w.u(v.muted?1:0,1);
    for(auto x:{v.applied_sequence,v.parameter_updates,v.saturations,v.dropped_transitions,v.resets})w.u(x,8);
}
inline void get(Reader& r,Status& v) noexcept {
    v.connection_id=r.u(8);v.audio_epoch=r.u(8);v.plugin_generation=r.u(8);v.aircraft_generation=r.u(8);
    v.generated_qpc=r.q();v.lease_expires_qpc=r.q();v.applied_sample_qpc=r.q();
    v.ready_groups=static_cast<std::uint32_t>(r.u(4));v.active_groups=static_cast<std::uint32_t>(r.u(4));v.held_groups=static_cast<std::uint32_t>(r.u(4));v.missing_groups=static_cast<std::uint32_t>(r.u(4));
    v.phase=static_cast<Phase>(r.u(1));v.reason=static_cast<Reason>(r.u(1));v.family=static_cast<Family>(r.u(1));auto m=r.u(1);if(m>1)r.ok=false;v.muted=m!=0;
    v.applied_sequence=r.u(8);v.parameter_updates=r.u(8);v.saturations=r.u(8);v.dropped_transitions=r.u(8);v.resets=r.u(8);
}
template<class T> bool encode_payload(const T& v,std::uint8_t* data,std::size_t size,std::size_t& used) noexcept {if(!valid(v))return false;Writer w{data,size};put(w,v);if(!w.ok)return false;used=w.pos;return true;}
template<class T> bool decode_payload(const std::uint8_t* data,std::size_t size,T& out) noexcept {T v{};Reader r{data,size};get(r,v);if(!r.ok||r.pos!=size||!valid(v))return false;out=v;return true;}
}
