#include "coordinator/audio.hpp"
#include <algorithm>
#include <limits>
namespace coordinator {
namespace a=psx::audio;namespace b=psx::bridge;
namespace {
std::uint32_t booleans(a::Group g) noexcept {
    switch(g){case a::Group::engines:{std::uint32_t mask{};for(unsigned i=0;i<4;++i)mask|=(1u<<(i*6+3))|(1u<<(i*6+4));return mask;}
    case a::Group::flaps:return 0xffc00;case a::Group::apu:return 12;case a::Group::packs:return 7;case a::Group::fuel:return 0xffffffffu;default:return 0;}
}
struct Predicates {std::uint32_t valid{},on{};};
Predicates predicates(const a::Snapshot& s,bool engine_phases) noexcept {
    Predicates p{booleans(s.group)&s.valid_fields,0};
    for(unsigned i=0;i<32;++i)if((p.valid&(1u<<i))&&s.values[i]!=0)p.on|=1u<<i;
    // Installed Sparky manifest predicates, evaluated from actual valid values.
    // The transition field names the predicate, never a fabricated spool value.
    if(engine_phases&&s.group==a::Group::engines&&s.family!=a::Family::unknown){
        for(unsigned base=0;base<24;base+=6){
            const auto n1=1u<<base,n2=1u<<(base+1),combustion=1u<<(base+3);
            if(s.valid_fields&n1){p.valid|=n1;if(s.values[base]<198)p.on|=n1;}
            if((s.valid_fields&(n2|combustion))==(n2|combustion)){
                p.valid|=n2;if(s.values[base+1]>20&&s.values[base+3]==0)p.on|=n2;
            }
        }
    }
    return p;
}
bool same(const a::Lease& x,const a::Lease& y) noexcept{return x.owner==y.owner&&x.operation==y.operation&&x.audio_epoch==y.audio_epoch&&x.expires_qpc==y.expires_qpc&&x.groups==y.groups;}
void input_codec(a::Writer& w,const AudioInput& v){w.u(static_cast<unsigned>(v.kind),1);w.u(static_cast<std::uint64_t>(v.now),8);for(auto x:{v.x,v.y,v.z,v.w})w.u(x,8);if(v.kind==AudioInputKind::snapshot)a::put(w,v.sample);else if(v.kind==AudioInputKind::status)a::put(w,v.status);else if(v.kind==AudioInputKind::ack)a::put(w,v.ack);}
}
const char* audio_group_name(a::Group g) noexcept {constexpr const char* names[]={"Engines","Gear","Flaps/slats","APU","Packs","Hydraulics","Fuel pumps","Airflow"};const auto i=static_cast<unsigned>(g);return i<8?names[i]:"Unknown";}
const char* audio_reason_name(a::Reason r) noexcept {constexpr const char* names[]={"None","Not following","Profile","Assets","Inputs","Suppression","Expired","Source lost","Epoch","Overflow","Disabled","ABI"};const auto i=static_cast<unsigned>(r);return i<12?names[i]:"Unknown";}
std::vector<std::uint8_t> encode_audio_input(const AudioInput& v){std::vector<std::uint8_t> bytes(512);a::Writer w{bytes.data(),bytes.size()};input_codec(w,v);bytes.resize(w.ok?w.pos:0);return bytes;}
bool decode_audio_input(const std::uint8_t* p,std::size_t n,AudioInput& v) noexcept {a::Reader r{p,n};AudioInput t;t.kind=static_cast<AudioInputKind>(r.u(1));t.now=r.q();t.x=r.u(8);t.y=r.u(8);t.z=r.u(8);t.w=r.u(8);if(t.kind==AudioInputKind::snapshot)a::get(r,t.sample);else if(t.kind==AudioInputKind::status)a::get(r,t.status);else if(t.kind==AudioInputKind::ack)a::get(r,t.ack);if(!r.ok||r.pos!=n||static_cast<unsigned>(t.kind)<1||static_cast<unsigned>(t.kind)>14)return false;v=t;return true;}
void AudioModel::reset(a::Reason reason,std::int64_t now) noexcept {
    ++view_.state.audio_epoch;++view_.revision;++view_.resets;view_.state.available_groups=0;
    view_.state.groups={};view_.status_available=false;view_.ownership={};view_.reason=reason;
    edge_head_=edge_count_=0;state_pending_=true;control_pending_=lease_pending_=false;waiting_stop_=stop_sent_=false;last_lease_=0;
    // Old ownership expires independently; an explicit stop for the new epoch
    // prevents an already queued acknowledgement from restoring old output.
    control(false,now);waiting_stop_=false;
}
void AudioModel::control(bool follow,std::int64_t now) noexcept {
    auto ownership=view_.ownership;if(!a::valid(ownership)||ownership.lease.audio_epoch!=view_.state.audio_epoch){ownership={};ownership.lease={owner_,++operation_,view_.state.audio_epoch,0,0};}
    pending_control_={view_.state.connection_id,view_.state.audio_epoch,view_.plugin.plugin_generation,view_.plugin.aircraft_generation,ownership,follow,view_.effective_muted()};
    control_pending_=true;if(!follow){waiting_stop_=view_.ownership.accepted_groups!=0||(pending_lease_.groups&&pending_lease_.audio_epoch==view_.state.audio_epoch&&pending_lease_.expires_qpc>now);stop_sent_=false;stop_qpc_=now;}
}
std::uint32_t AudioModel::fresh_groups(std::int64_t now) const noexcept {
    if(!frequency_||!view_.peer)return 0;std::uint32_t mask{};
    for(std::size_t i=0;i<a::group_count;++i)if(view_.state.available_groups&(1u<<i)){const auto& s=view_.state.groups[i];if(now>=s.received_qpc&&now-s.received_qpc<=static_cast<std::int64_t>(frequency_))mask|=1u<<i;}
    return mask;
}
void AudioModel::tick(std::int64_t now) noexcept {
    if(!frequency_||!owner_)return;
    while(edge_count_&&now>=edges_[edge_head_].sample_qpc&&now-edges_[edge_head_].sample_qpc>=static_cast<std::int64_t>(frequency_)){edge_head_=(edge_head_+1)%edges_.size();--edge_count_;}
    const bool status_current=view_.status_available&&now>=view_.status_received&&now-view_.status_received<=static_cast<std::int64_t>(frequency_);
    const auto groups=view_.following&&status_current&&!waiting_stop_?(view_.plugin.ready_groups&fresh_groups(now)&view_.state.suppression_capabilities):0u;
    if((view_.ownership.accepted_groups&~groups)&&!waiting_stop_)control(false,now);
    const bool released=waiting_stop_&&stop_sent_&&status_current&&!view_.plugin.active_groups&&view_.plugin.generated_qpc>=stop_qpc_;
    if(waiting_stop_&&!released)return;
    if(lease_pending_)return;
    if(!groups&&!released)return;
    if(last_lease_&&now-last_lease_<static_cast<std::int64_t>(frequency_*a::renewal_ms/1000)&&!released)return;
    if(now>std::numeric_limits<std::int64_t>::max()-static_cast<std::int64_t>(frequency_))return;
    pending_lease_={owner_,++operation_,view_.state.audio_epoch,groups?now+static_cast<std::int64_t>(frequency_):0,groups};lease_pending_=true;last_lease_=now;
    if(released){waiting_stop_=false;stop_sent_=false;view_.ownership={};}
}
bool AudioModel::apply(const AudioInput& v) noexcept {
    if(v.now<0)return false;
    switch(v.kind){
    case AudioInputKind::attach:if(!v.x)return false;view_.state.connection_id=v.x;reset(a::Reason::epoch,v.now);view_.plugin={};break;
    case AudioInputKind::detach:view_.following=false;reset(a::Reason::disabled,v.now);break;
    case AudioInputKind::hello:if(!(v.x||v.y)||((v.z|v.w)&~a::all_groups))return false;reset(a::Reason::epoch,v.now);view_.peer=true;view_.state.producer_high=v.x;view_.state.producer_low=v.y;advertised_=static_cast<std::uint32_t>(v.z);view_.state.suppression_capabilities=static_cast<std::uint32_t>(v.w);lifecycle_=configuration_=0;family_=a::Family::unknown;view_.native_enqueued=view_.native_sent=view_.native_dropped=0;watermarks_={};watermark_groups_=0;break;
    case AudioInputKind::lost:view_.peer=false;reset(a::Reason::source_lost,v.now);view_.state.suppression_capabilities=0;break;
    case AudioInputKind::boundary:reset(a::Reason::epoch,v.now);break;
    case AudioInputKind::overflow:++view_.overflows;reset(a::Reason::overflow,v.now);break;
    case AudioInputKind::health:{const auto groups=static_cast<std::uint32_t>(v.w),capabilities=static_cast<std::uint32_t>(v.w>>32);
        if(!view_.peer||((groups|capabilities)&~a::all_groups)||v.y>v.x||v.z<view_.native_dropped)return false;
        if(v.z>view_.native_dropped){++view_.overflows;reset(a::Reason::overflow,v.now);}else if(groups!=advertised_||capabilities!=view_.state.suppression_capabilities)reset(a::Reason::suppression,v.now);
        view_.native_enqueued=v.x;view_.native_sent=v.y;view_.native_dropped=v.z;advertised_=groups;view_.state.suppression_capabilities=capabilities;state_pending_=true;break;}
    case AudioInputKind::snapshot:{
        ++view_.received;auto s=v.sample;s.received_qpc=v.now;
        if(!view_.peer||!a::valid(s)||s.source!=a::Source::exporter||!(advertised_&a::bit(s.group))||s.sample_end_qpc>v.now||s.lifecycle_epoch<lifecycle_||s.configuration_epoch<configuration_){++view_.rejected;return true;}
        const auto bits=booleans(s.group)&s.valid_fields;for(unsigned i=0;i<32;++i)if((bits&(1u<<i))&&s.values[i]!=0&&s.values[i]!=1){++view_.rejected;return true;}
        const bool changed=lifecycle_&&(lifecycle_!=s.lifecycle_epoch||configuration_!=s.configuration_epoch||family_!=s.family||variant_!=s.variant);
        if(changed)reset(a::Reason::epoch,v.now);lifecycle_=s.lifecycle_epoch;configuration_=s.configuration_epoch;family_=s.family;variant_=s.variant;
        const auto index=static_cast<std::size_t>(s.group);const auto& previous=watermarks_[index];
        const bool baseline=(view_.state.available_groups&a::bit(s.group))!=0;
        const bool retained=(watermark_groups_&a::bit(s.group))&&s.lifecycle_epoch==previous.lifecycle_epoch&&s.configuration_epoch==previous.configuration_epoch&&s.family==previous.family&&s.variant==previous.variant;
        if(retained&&s.sequence==previous.sequence&&(s.flags&a::held)&&s.sample_start_qpc==previous.sample_start_qpc&&s.sample_end_qpc==previous.sample_end_qpc&&s.valid_fields==previous.valid_fields&&s.values==previous.values&&((s.flags^previous.flags)&~a::held)==0){view_.state.groups[index]=watermarks_[index]=s;view_.state.available_groups|=a::bit(s.group);++view_.revision;state_pending_=true;return true;}
        if(retained&&(s.sequence<=previous.sequence||s.sample_end_qpc<previous.sample_end_qpc)){++view_.rejected;return true;}
        const auto current=predicates(s,checkpoint_version_>=2),prior=predicates(previous,checkpoint_version_>=2);
        std::uint32_t changes{};if(baseline&&s.sample_end_qpc-previous.sample_end_qpc<static_cast<std::int64_t>(frequency_))changes=current.valid&prior.valid&(current.on^prior.on);
        std::size_t count{};for(unsigned i=0;i<32;++i)if(changes&(1u<<i))++count;
        if(count>edges_.size()-edge_count_){++view_.overflows;reset(a::Reason::overflow,v.now);changes=0;}
        for(unsigned i=0;i<32;++i)if(changes&(1u<<i)){auto& edge=edges_[(edge_head_+edge_count_)%edges_.size()];edge={view_.state.connection_id,view_.state.audio_epoch,++transition_sequence_,s.sequence,s.sample_end_qpc,s.group,static_cast<std::uint8_t>(i),(current.on&(1u<<i))?a::Edge::rising:a::Edge::falling};++edge_count_;++view_.transitions;}
        view_.state.groups[index]=watermarks_[index]=s;watermark_groups_|=a::bit(s.group);view_.state.available_groups|=a::bit(s.group);++view_.revision;state_pending_=true;break;}
    case AudioInputKind::follow:if(v.x>1)return false;if(view_.following==(v.x!=0))break;view_.following=v.x!=0;if(!view_.following)control(false,v.now);else {view_.reason=a::Reason::inputs;waiting_stop_=false;}break;
    case AudioInputKind::mute:if(v.x>1)return false;view_.muted=v.x!=0;if(waiting_stop_)pending_control_.muted=view_.effective_muted();else if(a::valid(view_.ownership))control(view_.following,v.now);break;
    case AudioInputKind::pause:{
        // x = qualified Main observation, y = Qi129 mask. Only bit zero is
        // full Pause; Motion Freeze must not silence otherwise running systems.
        if(checkpoint_version_<3||v.x>1||v.y>3||(!v.x&&v.y))return false;
        const bool before=view_.effective_muted();view_.pause_available=v.x!=0;if(v.x)view_.psx_paused=(v.y&1)!=0;
        if(before!=view_.effective_muted()){
            if(waiting_stop_)pending_control_.muted=view_.effective_muted();
            else if(a::valid(view_.ownership))control(view_.following,v.now);
        }
        break;}
    case AudioInputKind::status:{const auto& s=v.status;if(!a::valid(s)||s.connection_id!=view_.state.connection_id||s.generated_qpc>v.now)return false;
        if(view_.plugin.plugin_generation&&(s.plugin_generation!=view_.plugin.plugin_generation||s.aircraft_generation!=view_.plugin.aircraft_generation)){view_.plugin=s;reset(a::Reason::epoch,v.now);return true;}
        if(s.audio_epoch!=view_.state.audio_epoch||!s.plugin_generation||!s.aircraft_generation||(view_.status_available&&s.generated_qpc<view_.plugin.generated_qpc))return true;
        view_.plugin=s;view_.status_received=v.now;view_.status_available=true;break;}
    case AudioInputKind::ack:{const auto& ack=v.ack;if(!a::valid(ack)||!same(ack.lease,pending_lease_)||ack.lease.audio_epoch!=view_.state.audio_epoch||ack.lease.owner!=owner_||lease_pending_){++view_.late_acks;return true;}
        if(ack.reason!=a::Reason::none){view_.reason=ack.reason;return true;}
        if(ack.accepted_groups&&(!view_.following||waiting_stop_||v.now>=ack.lease.expires_qpc||!view_.status_available||v.now-view_.status_received>static_cast<std::int64_t>(frequency_)||(ack.accepted_groups&(fresh_groups(v.now)&view_.plugin.ready_groups))!=ack.accepted_groups)){++view_.late_acks;return true;}
        view_.ownership=ack;view_.reason=ack.accepted_groups?a::Reason::none:a::Reason::not_following;control(ack.accepted_groups&&view_.following,v.now);if(!ack.accepted_groups){waiting_stop_=false;stop_sent_=false;}break;}
    case AudioInputKind::tick:tick(v.now);break;
    default:return false;
    }
    return true;
}
bool AudioModel::next_lease(a::Lease& out) noexcept {if(!lease_pending_)return false;out=pending_lease_;lease_pending_=false;return true;}
bool AudioModel::next_frame(b::Frame& out,std::int64_t now) noexcept {
    // Publish new epoch/baselines before any controls or retained transitions.
    if(state_pending_&&published_epoch_!=view_.state.audio_epoch){state_pending_=false;published_epoch_=view_.state.audio_epoch;return b::encode(view_.state,0,out);}
    if(control_pending_){control_pending_=false;if(!pending_control_.follow){stop_sent_=true;stop_qpc_=now;}return b::encode(pending_control_,0,out);}
    if(edge_count_){const auto edge=edges_[edge_head_];edge_head_=(edge_head_+1)%edges_.size();--edge_count_;return b::encode(edge,0,out);}
    if(state_pending_){state_pending_=false;return b::encode(view_.state,0,out);}
    return false;
}
std::vector<std::uint8_t> AudioModel::checkpoint() const {
    std::vector<std::uint8_t> bytes(16384);a::Writer w{bytes.data(),bytes.size()};w.u(checkpoint_version_,1);
    for(auto x:{frequency_,owner_,operation_,transition_sequence_,lifecycle_,configuration_,published_epoch_})w.u(x,8);
    w.u(static_cast<unsigned>(family_),1);w.u(variant_,1);w.u(advertised_,4);w.u(static_cast<std::uint64_t>(last_lease_),8);w.u(static_cast<std::uint64_t>(stop_qpc_),8);
    for(bool x:{state_pending_,control_pending_,lease_pending_,waiting_stop_,stop_sent_,view_.peer,view_.following,view_.muted,view_.status_available})w.u(x?1:0,1);
    a::put(w,view_.state);a::put(w,view_.plugin);a::put(w,view_.ownership);w.u(static_cast<std::uint64_t>(view_.status_received),8);
    for(auto x:{view_.received,view_.rejected,view_.transitions,view_.overflows,view_.resets,view_.late_acks,view_.revision,view_.native_enqueued,view_.native_sent,view_.native_dropped})w.u(x,8);w.u(static_cast<unsigned>(view_.reason),1);
    a::put(w,pending_lease_);a::put(w,pending_control_);w.u(edge_count_,1);for(std::size_t i=0;i<edge_count_;++i)a::put(w,edges_[(edge_head_+i)%edges_.size()]);w.u(watermark_groups_,4);for(std::size_t i=0;i<a::group_count;++i)if(watermark_groups_&(1u<<i))a::put(w,watermarks_[i]);
    if(checkpoint_version_>=3){w.u(view_.pause_available?1:0,1);w.u(view_.psx_paused?1:0,1);}bytes.resize(w.ok?w.pos:0);return bytes;
}
bool AudioModel::restore(const std::uint8_t* p,std::size_t n){
    a::Reader r{p,n};AudioModel t;const auto version=r.u(1);if(version!=1&&version!=2&&version!=3)return false;t.checkpoint_version_=static_cast<std::uint8_t>(version);t.frequency_=r.u(8);t.owner_=r.u(8);t.operation_=r.u(8);t.transition_sequence_=r.u(8);t.lifecycle_=r.u(8);t.configuration_=r.u(8);t.published_epoch_=r.u(8);t.family_=static_cast<a::Family>(r.u(1));t.variant_=static_cast<std::uint8_t>(r.u(1));t.advertised_=static_cast<std::uint32_t>(r.u(4));t.last_lease_=r.q();t.stop_qpc_=r.q();
    bool* flags[]={&t.state_pending_,&t.control_pending_,&t.lease_pending_,&t.waiting_stop_,&t.stop_sent_,&t.view_.peer,&t.view_.following,&t.view_.muted,&t.view_.status_available};for(auto* f:flags){auto x=r.u(1);if(x>1)return false;*f=x!=0;}
    a::get(r,t.view_.state);a::get(r,t.view_.plugin);a::get(r,t.view_.ownership);t.view_.status_received=r.q();
    std::uint64_t* counters[]={&t.view_.received,&t.view_.rejected,&t.view_.transitions,&t.view_.overflows,&t.view_.resets,&t.view_.late_acks,&t.view_.revision,&t.view_.native_enqueued,&t.view_.native_sent,&t.view_.native_dropped};for(auto* x:counters)*x=r.u(8);t.view_.reason=static_cast<a::Reason>(r.u(1));
    a::get(r,t.pending_lease_);a::get(r,t.pending_control_);t.edge_count_=static_cast<std::size_t>(r.u(1));if(t.edge_count_>t.edges_.size())return false;for(std::size_t i=0;i<t.edge_count_;++i){a::get(r,t.edges_[i]);if(!a::valid(t.edges_[i])||t.edges_[i].audio_epoch!=t.view_.state.audio_epoch)return false;}
    t.watermark_groups_=static_cast<std::uint32_t>(r.u(4));if(t.watermark_groups_&~a::all_groups)return false;for(std::size_t i=0;i<a::group_count;++i)if(t.watermark_groups_&(1u<<i)){a::get(r,t.watermarks_[i]);if(!a::valid(t.watermarks_[i])||static_cast<std::size_t>(t.watermarks_[i].group)!=i)return false;}
    if(version>=3){const auto available=r.u(1),paused=r.u(1);if(available>1||paused>1)return false;t.view_.pause_available=available!=0;t.view_.psx_paused=paused!=0;}
    if(!r.ok||r.pos!=n||!t.frequency_||(frequency_&&t.frequency_!=frequency_)||!t.owner_||!a::valid(t.view_.state)||static_cast<unsigned>(t.family_)>3||static_cast<unsigned>(t.view_.reason)>11||(t.advertised_&~a::all_groups)||(t.view_.status_available&&!a::valid(t.view_.plugin))||(t.control_pending_&&!a::valid(t.pending_control_))||(t.lease_pending_&&!a::valid(t.pending_lease_)))return false;
    *this=t;return true;
}
}
