#include "psx_plugin/audio_renderer.hpp"
#include <psx/sync/runtime.hpp>
#include <algorithm>
#include <cstring>

namespace psx::xp {
bool relative_cockpit_attributes(const AudioAttributes& listener,motion::Vec3 point,motion::Vec3 eye,AudioAttributes& out) noexcept {
    if(!std::isfinite(listener.velocity.x)||!std::isfinite(listener.velocity.y)||!std::isfinite(listener.velocity.z))return false;
    using motion::Vec3;const Vec3 forward{listener.forward.x,listener.forward.y,listener.forward.z},up{listener.up.x,listener.up.y,listener.up.z};
    Vec3 f,u,r;if(!motion::normalize(forward,f)||!motion::normalize(up-f*motion::dot(up,f),u)||!motion::normalize(motion::cross(f,u),r))return false;
    const auto delta=point-eye;const auto p=Vec3{listener.position.x,listener.position.y,listener.position.z}+r*delta.x+u*delta.y-f*delta.z;
    if(!motion::finite(p))return false;out=listener;out.position={static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)};
    // Same instantaneous velocity as listener: no invented relative Doppler.
    out.forward={static_cast<float>(f.x),static_cast<float>(f.y),static_cast<float>(f.z)};
    out.up={static_cast<float>(u.x),static_cast<float>(u.y),static_cast<float>(u.z)};return true;
}
bool AudioRenderer::gate(Event& e,bool mute) noexcept {
    if(!e.group)return true; // Snapshots have no owned voice group.
    if(e.gate_known&&e.core_muted==mute)return true;
    int observed{};
    if(api_.group_mute(e.group,mute?1:0)||api_.group_get_mute(e.group,&observed)||(observed!=0)!=mute){e.gate_known=false;return false;}
    e.gate_known=true;e.core_muted=mute;return true;
}
void AudioRenderer::request_stop(Event& e) noexcept {
    if(!e.instance||(!e.playing&&!e.pending_stop))return;
    if(!gate(e,true))e.failed=true;
    e.pending_stop=true;
    if(!e.stop_sent)e.stop_sent=api_.stop(e.instance,1)==0;
    e.baseline=false;
}
void AudioRenderer::service_stops() noexcept {
    for(auto& e:events_)if(e.pending_stop){
        request_stop(e);int state{};
        if(e.stop_sent&&!api_.playback(e.instance,&state)&&state==2){e.pending_stop=false;e.stop_sent=false;e.playing=false;}
    }
}
std::uint32_t AudioRenderer::pending_groups() const noexcept {
    std::uint32_t mask{};for(std::size_t i=0;i<audio_event_count;++i)if(events_[i].pending_stop)
        mask|=sparky_events[i].trigger==Trigger::snapshot?snapshot_scope_:audio::bit(sparky_events[i].group);
    return mask;
}
void AudioRenderer::pending_status() noexcept {
    const auto pending=pending_groups();status_.active_groups|=pending;
    if(pending){status_.phase=audio::Phase::held;if(status_.reason==audio::Reason::none)status_.reason=audio::Reason::assets;}
}
void AudioRenderer::stop_all() noexcept {
    for(auto& e:events_){request_stop(e);e.baseline=false;e.last_sequence=0;}
    service_stops();status_.active_groups=pending_groups();transition_count_=0;
}
void AudioRenderer::release_event(Event& e) noexcept {
    if(e.instance&&api_.release)api_.release(e.instance);
    if(e.description&&e.loaded&&api_.unload_samples)api_.unload_samples(e.description);
    e={};
}
void AudioRenderer::release() noexcept {
    stop_all();for(auto& e:events_)release_event(e);retry_after_=0;was_follow_=false;
}
void AudioRenderer::host(const AudioHost& h) noexcept {
    if(h.plugin_generation!=host_.plugin_generation||h.aircraft_generation!=host_.aircraft_generation||h.profile.sha256!=host_.profile.sha256){release();control_={};owner_=operation_=0;gate_.reset();status_.applied_sequence=0;status_.applied_sample_qpc=0;++status_.resets;}
    if(h.connection!=host_.connection){stop_all();gate_.reset();control_={};owner_=operation_=0;faulted_=false;blocked_epoch_=0;status_.applied_sequence=0;status_.applied_sample_qpc=0;++status_.resets;}
    if(!h.connected||!h.follow){stop_all();control_={};was_follow_=false;}
    host_=h;status_.connection_id=h.connection;status_.plugin_generation=h.plugin_generation;status_.aircraft_generation=h.aircraft_generation;
}
bool AudioRenderer::state(const audio::State& s) noexcept {
    const auto previous=gate_.value().audio_epoch;
    if(!gate_.accept(s,host_.connection)){fault();return false;}
    if(s.audio_epoch!=previous)for(auto& history:histories_)history.clear();
    if(psx::sync::enabled())for(unsigned g=0;g<audio::group_count;++g)if(gate_.value().available_groups&(1u<<g)){
        const auto& source=gate_.value().groups[g];psx::sync::Values value;
        value.producer=s.producer_high;value.producer_low=s.producer_low;value.lifecycle=source.lifecycle_epoch;
        value.configuration=source.configuration_epoch;value.sequence=source.sequence;value.qpc=source.sample_end_qpc?source.sample_end_qpc:source.received_qpc;
        value.valid=source.valid_fields;value.value=source.values;
        value.continuous=psx::sync::audio_continuous_mask(g);
        histories_[g].append(value);
    }
    if(s.audio_epoch!=previous){stop_all();control_={};owner_=operation_=0;status_.applied_sequence=0;status_.applied_sample_qpc=0;++status_.resets;if(s.audio_epoch>blocked_epoch_)faulted_=false;}
    status_.audio_epoch=s.audio_epoch;status_.family=s.groups[0].family;return true;
}
bool AudioRenderer::control(const audio::Control& c,std::int64_t now) noexcept {
    if(!audio::valid(c)||!host_.connected||c.connection_id!=host_.connection||c.audio_epoch!=gate_.value().audio_epoch||
        c.plugin_generation!=host_.plugin_generation||c.aircraft_generation!=host_.aircraft_generation||
        (owner_&&owner_!=c.ownership.lease.owner)||c.ownership.lease.operation<=operation_)return false;
    const auto expiry=c.ownership.lease.expires_qpc;
    if(c.ownership.accepted_groups&&(expiry<=now||expiry-now>static_cast<std::int64_t>(frequency_*audio::lease_ms/1000)))return false;
    owner_=c.ownership.lease.owner;operation_=c.ownership.lease.operation;control_=c;
    if(!c.follow)stop_all();return true;
}
void AudioRenderer::transition(const audio::Transition& t) noexcept {
    if(!audio::valid(t)||t.connection_id!=host_.connection||t.audio_epoch!=gate_.value().audio_epoch){++status_.dropped_transitions;return;}
    if(transition_count_==transitions_.size()){fault();return;}transitions_[transition_count_++]=t;
}
void AudioRenderer::fault() noexcept {stop_all();faulted_=true;blocked_epoch_=gate_.value().audio_epoch;++status_.dropped_transitions;status_.reason=audio::Reason::overflow;}
bool AudioRenderer::prepare(std::size_t i) noexcept {
    auto& e=events_[i];if(e.failed)return false;
    if(e.instance){
        if(sparky_events[i].is3d&&!e.group){if(api_.channel_group(e.instance,&e.group)||!e.group)return false;if(!gate(e,true)){e.failed=true;return false;}}
        return true;
    }
    if(!api_.studio||!api_.event)return false;
    if(!e.description){
        AudioGuid expected{};std::memcpy(&expected,sparky_events[i].guid.data(),16);
        if(api_.event(api_.studio,&expected,&e.description)||!e.description){e.failed=true;return false;}
        AudioGuid id{};int dimensions{};
        if(api_.event_id(e.description,&id)||std::memcmp(&id,sparky_events[i].guid.data(),16)||api_.is3d(e.description,&dimensions)||
            (dimensions!=0)!=sparky_events[i].is3d){e.failed=true;return false;}
        int count{},one{};if(api_.parameter_count(e.description,&count)||count<0||count>static_cast<int>(audio_parameter_capacity)||api_.oneshot(e.description,&one)||
            (one!=0)!=sparky_events[i].oneshot){e.failed=true;return false;}
        e.oneshot=one!=0;
        for(int p=0;p<count;++p){AudioParameterDescription d;if(api_.parameter(e.description,p,&d)||!d.name||!std::isfinite(d.minimum)||!std::isfinite(d.maximum)||d.minimum>d.maximum){e.failed=true;return false;}
            if(d.type!=0){if(!(d.flags&1u)){e.failed=true;return false;}continue;} // Automatic Distance/Cone never written.
            const auto source=parameter_source(d.name,sparky_events[i]);
            if(d.flags||source.kind==Input::Kind::missing){e.failed=true;return false;}
            e.parameters[e.count++]={d.id,source,d.minimum,d.maximum,0,false};
        }
        if(api_.load_samples(e.description)){e.failed=true;return false;}
        e.loaded=true;
    }
    int loading{};if(api_.sample_state(e.description,&loading)){e.failed=true;return false;}
    if(loading==4)e.failed=true;
    if(loading!=3)return false; // Never flush/wait synchronously for samples.
    if(api_.create(e.description,&e.instance)||!e.instance){e.failed=true;return false;}
    if(sparky_events[i].is3d){if(api_.channel_group(e.instance,&e.group)||!e.group)return false;if(!gate(e,true)){e.failed=true;return false;}}
    return true;
}
void AudioRenderer::tick(std::int64_t now) noexcept {
    service_stops();
    status_.generated_qpc=now;status_.ready_groups=0;status_.missing_groups=0;status_.held_groups=0;
    auto s=gate_.value();
    if(psx::sync::enabled()){const auto limit=psx::sync::freshness_now(now);for(unsigned g=0;g<audio::group_count;++g)if(limit<s.groups[g].sample_end_qpc||limit-s.groups[g].sample_end_qpc>=static_cast<std::int64_t>(frequency_))s.available_groups&=~(1u<<g);}
    if(psx::sync::enabled()&&now>=static_cast<std::int64_t>(frequency_/40))for(unsigned g=0;g<audio::group_count;++g){
        psx::sync::Presented p;if((s.available_groups&(1u<<g))&&histories_[g].evaluate(now-static_cast<std::int64_t>(frequency_/40),p))for(unsigned i=0;i<32;++i)if(p.sample.valid&s.groups[g].valid_fields&(1u<<i))s.groups[g].values[i]=p.sample.value[i];
    }
    status_.audio_epoch=s.audio_epoch;status_.muted=control_.muted;status_.lease_expires_qpc=control_.ownership.lease.expires_qpc;
    if(!host_.connected||!s.audio_epoch){stop_all();status_.phase=audio::Phase::inactive;status_.reason=audio::Reason::source_lost;pending_status();return;}
    if(faulted_){stop_all();status_.phase=audio::Phase::unavailable;status_.reason=audio::Reason::overflow;pending_status();return;}
    if(!host_.profile.qualified||!host_.profile.audio_qualified||host_.profile.generation!=host_.aircraft_generation){stop_all();status_.phase=audio::Phase::unavailable;status_.reason=audio::Reason::profile;pending_status();return;}
    if(!api_.studio||!api_.event){stop_all();status_.phase=audio::Phase::unavailable;status_.reason=audio::Reason::abi;pending_status();return;}
    std::array<Evaluation,audio_event_count> conditions{};std::array<bool,audio_event_count> ready{};
    std::uint32_t present{},missing{};bool snapshots=true;bool asset_missing=false;
    if(now>=retry_after_){for(auto& e:events_)if(e.failed&&!e.pending_stop)release_event(e);retry_after_=now+static_cast<std::int64_t>(frequency_);}
    for(std::size_t i=0;i<audio_event_count;++i){const auto& spec=sparky_events[i];if(!event_family(spec,status_.family))continue;
        const bool snapshot=spec.trigger==Trigger::snapshot;bool good=prepare(i);asset_missing=asset_missing||!good;
        conditions[i]=evaluate(spec,s);good=good&&conditions[i].available;
        for(std::size_t p=0;p<events_[i].count;++p){float value{};good=good&&input_value(events_[i].parameters[p].source,s,host_,value);}
        ready[i]=good;if(snapshot)snapshots=snapshots&&good;else {present|=audio::bit(spec.group);if(!good)missing|=audio::bit(spec.group);}
    }
    status_.missing_groups=missing|(audio::all_groups&~present);
    status_.ready_groups=snapshots?(present&~missing):0;
    std::uint32_t active{};
    if(host_.follow&&control_.follow&&control_.connection_id==host_.connection&&control_.audio_epoch==s.audio_epoch)
        for(unsigned g=0;g<audio::group_count;++g)if(audio::owns(control_.ownership,now,s.audio_epoch,static_cast<audio::Group>(g)))active|=1u<<g;
    active&=status_.ready_groups&s.suppression_capabilities;
    if(active)snapshot_scope_=active;
    AudioAttributes listener{};AudioVector attenuation{};
    if(active&&api_.listener(api_.studio,0,&listener,&attenuation)){active=0;asset_missing=true;}
    const bool fresh_follow=active&&!was_follow_;was_follow_=active!=0;
    std::uint32_t failed_groups{};bool snapshot_failed=false;
    // Edges can precede their replaceable state; retain them until that state arrives.
    // Expired/obsolete/coalesced actionable edges are counted once, never burst-replayed.
    std::size_t usable{};
    for(std::size_t t=0;t<transition_count_;++t){const auto& edge=transitions_[t];
        if(edge.sample_qpc<=now&&now-edge.sample_qpc<=static_cast<std::int64_t>(frequency_*audio::renewal_ms/1000))transitions_[usable++]=edge;
        else ++status_.dropped_transitions;
    }transition_count_=usable;
    std::array<bool,audio::transition_capacity> actionable{},consumed{};
    for(std::size_t i=0;i<audio_event_count;++i){auto& e=events_[i];const auto& spec=sparky_events[i];const bool snapshot=spec.trigger==Trigger::snapshot;
        if(event_family(spec,status_.family))for(std::size_t t=0;t<transition_count_;++t)
            actionable[t]=actionable[t]||transition_matches(spec,transitions_[t]);
        const bool allowed=ready[i]&&(snapshot?active!=0:(active&audio::bit(spec.group))!=0)&&event_family(spec,status_.family);
        if(!allowed){request_stop(e);e.baseline=false;continue;}
        if(e.pending_stop)continue; // Never unmute or restart before the prior stop is processed.
        const auto& group=s.groups[static_cast<std::size_t>(spec.group)];
        bool error=false;
        for(std::size_t p=0;p<e.count;++p){auto& param=e.parameters[p];float value{};if(!input_value(param.source,s,host_,value)){error=true;break;}
            const auto limited=std::clamp(value,param.low,param.high);
            if(!param.sent||value!=param.last){if(value!=limited)++status_.saturations;if(api_.set_parameter(e.instance,param.id,limited,1)){error=true;break;}param.last=value;param.sent=true;++status_.parameter_updates;}}
        AudioAttributes position;if(spec.is3d&&!relative_cockpit_attributes(listener,spec.position,host_.profile.eye,position))error=true;
        if(spec.is3d&&!error&&(!e.positioned||std::memcmp(&position,&e.attributes,sizeof(position)))){
            if(api_.set_attributes(e.instance,&position))error=true;else {e.attributes=position;e.positioned=true;}}
        const float volume=control_.muted&&!snapshot?0.0F:1.0F;
        if(!snapshot&&!gate(e,control_.muted))error=true;
        if(!error&&e.volume!=volume){if(api_.set_volume(e.instance,volume))error=true;else e.volume=volume;}
        const bool on=conditions[i].on;bool fire=false;std::size_t selected=transition_count_;
        if(e.baseline&&!fresh_follow){
            if(spec.trigger==Trigger::pack_stop)fire=on&&!e.last_on&&group.sequence>e.last_sequence;
            // Pack pressure can already trigger its stop from current state; a later
            // valve edge must not replay that same stop. Other edge events use only records.
            const auto edge_floor=spec.trigger==Trigger::pack_stop?e.last_sequence:e.baseline_sequence;
            for(std::size_t t=0;t<transition_count_;++t){const auto& edge=transitions_[t];
                if(edge.source_sequence>edge_floor&&edge.source_sequence<=group.sequence&&transition_matches(spec,edge)&&on&&
                    (selected==transition_count_||edge.source_sequence>transitions_[selected].source_sequence||
                    (edge.source_sequence==transitions_[selected].source_sequence&&edge.sequence>transitions_[selected].sequence)))selected=t;
            }
            fire=fire||selected!=transition_count_;
        }
        if(e.playing){int playback{};if(api_.playback(e.instance,&playback))error=true;else if(playback==2)e.playing=false;}
        if(!error){
            if(edge_event(spec.trigger)){if(fire&&!control_.muted){if(api_.start(e.instance))error=true;else e.playing=true;}}
            else if(on&&(!e.playing||(spec.trigger==Trigger::engine_crank&&selected!=transition_count_&&transitions_[selected].source_sequence>e.last_start_sequence))&&
                (!e.oneshot||(e.baseline&&!fresh_follow&&!e.last_on))){if(api_.start(e.instance))error=true;else {e.playing=true;e.last_start_sequence=group.sequence;}}
            else if(!on&&e.playing)request_stop(e);
        }
        if(selected!=transition_count_&&!error&&!control_.muted)consumed[selected]=true;
        if(error){request_stop(e);e.failed=true;
            if(snapshot)snapshot_failed=true;else failed_groups|=audio::bit(spec.group);}
        if(!e.baseline||fresh_follow)e.baseline_sequence=group.sequence;
        e.baseline=true;e.last_on=on;e.last_sequence=group.sequence;
        if(!snapshot){status_.applied_sample_qpc=(std::max)(status_.applied_sample_qpc,group.sample_end_qpc);status_.applied_sequence=(std::max)(status_.applied_sequence,group.sequence);if(group.flags&audio::held)status_.held_groups|=audio::bit(spec.group);}
    }
    if(snapshot_failed)failed_groups=audio::all_groups;
    if(failed_groups){active&=~failed_groups;status_.ready_groups&=~failed_groups;status_.missing_groups|=failed_groups;asset_missing=true;
        for(std::size_t i=0;i<audio_event_count;++i){auto& e=events_[i];const auto& spec=sparky_events[i];
            if((failed_groups&audio::bit(spec.group))||(spec.trigger==Trigger::snapshot&&!active)){
                request_stop(e);e.baseline=false;}}
    }
    std::size_t retained{};
    for(std::size_t t=0;t<transition_count_;++t){const auto& edge=transitions_[t];
        if((s.available_groups&audio::bit(edge.group))&&edge.source_sequence>s.groups[static_cast<std::size_t>(edge.group)].sequence)
            transitions_[retained++]=edge;
        else if(actionable[t]&&!consumed[t])++status_.dropped_transitions;
    }
    transition_count_=retained;status_.active_groups=active;status_.held_groups&=active;
    if(active){status_.phase=control_.muted?audio::Phase::muted:audio::Phase::active;status_.reason=audio::Reason::none;}
    else {status_.phase=audio::Phase::inactive;status_.reason=!host_.follow?audio::Reason::not_following:
        (!status_.ready_groups?(asset_missing?audio::Reason::assets:audio::Reason::inputs):
        (control_.ownership.lease.expires_qpc&&now>=control_.ownership.lease.expires_qpc?audio::Reason::expired:audio::Reason::suppression));}
    service_stops();pending_status();
}
} // namespace psx::xp
