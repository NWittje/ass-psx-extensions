#include "psx_plugin/status_source.hpp"

namespace psx::xp {
void StatusSource::begin(SdkApi& sdk,motion::InstanceId producer,std::uint64_t frequency,
        std::int64_t (*clock)() noexcept,Sink sink,void* context) noexcept {
    *this=StatusSource{};sdk_=&sdk;producer_=producer;frequency_=frequency;clock_=clock;sink_=sink;context_=context;
    if(sdk.ready()==Status::ok)bind();
}
void StatusSource::bind() noexcept {
    sdk_generation_=sdk_->generation();async_=nullptr;
    auto ref=XPLMFindDataRef("sim/graphics/scenery/async_scenery_load_in_progress");
    if(sdk_->inspect(ref,xplmType_Int,false)==Status::ok&&XPLMGetDataRefTypes(ref)==xplmType_Int&&
       !XPLMCanWriteDataRef(ref)){
        XPLMDataRefInfo_t info{};info.structSize=sizeof(info);XPLMGetDataRefInfo(ref,&info);
        if(info.owner==XPLM_PLUGIN_XPLANE)async_=ref;
    }
}
void StatusSource::observe(const StatusContext& context,const MotionController& motion,bool forced,std::uint32_t notification) noexcept {
    if(!sdk_||!sdk_->on_thread()||!clock_||!frequency_)return;
    try {
        const auto now=clock_();const auto facts=motion.readiness();const auto& report=motion.report().status;
        status::Snapshot value{};value.producer=producer_;value.role=status::Role::plugin;
        // Placeholder for local validation; transport binds its authenticated subscription ID.
        value.connection_id=1;value.enable_generation=context.enable;value.aircraft_generation=context.aircraft;
        value.scenery_generation=context.scenery;value.local_frame_generation=context.local_frame;
        value.callback_count=context.callback_count;value.command_id=report.operation_id;
        value.placement_attempt=facts.attempt;value.phase_start_qpc=facts.started_qpc;value.call_returned_qpc=facts.returned_qpc;
        value.notification_kind=notification?notification:latest_.notification_kind;
        value.notification_qpc=notification?now:latest_.notification_qpc;
        const auto& target=facts.target;
        value.target_source=static_cast<std::uint32_t>(!facts.attempt&&!target.source.boost_sequence&&!target.provenance.right_sequence?
            status::TargetSource::none:target.provenance.kind==motion::PoseSource::exporter?
            status::TargetSource::exporter:status::TargetSource::public_main_boost);
        value.target_producer=target.provenance.kind==motion::PoseSource::exporter?target.provenance.domain.producer:motion::InstanceId{};
        value.target_epoch=target.provenance.kind==motion::PoseSource::exporter?target.provenance.domain.pose_epoch:target.source.motion_epoch;
        value.target_sequence=target.provenance.kind==motion::PoseSource::exporter?target.provenance.right_sequence:target.source.boost_sequence;
        value.selection_epoch=target.provenance.selection_epoch;
        if(context.enabled)value.flags|=status::plugin_enabled;
        if(context.aircraft_loaded)value.flags|=status::aircraft_loaded;
        if(context.enabled&&facts.bindings)value.flags|=status::bindings_ready;
        if(context.enabled&&facts.required)value.flags|=status::readback_required;
        if(context.enabled&&facts.reference)value.flags|=status::reference_ready;
        if(context.enabled&&(report.flags&motion::readback_valid))value.flags|=status::readback_ready;
        if(context.enabled&&(facts.call_active||report.phase==motion::Phase::relocating))value.flags|=status::placement_active;
        if(context.enabled&&facts.returned_qpc)value.flags|=status::placement_returned;
        if(context.enabled&&facts.reconciling)value.flags|=status::reconciling;
        constexpr auto volatile_flags=status::qualified|status::async_available|status::async_busy|
            status::bindings_ready|status::reference_ready|status::readback_ready;
        const bool same_core=latest_.sequence&&value.enable_generation==latest_.enable_generation&&
            value.aircraft_generation==latest_.aircraft_generation&&value.scenery_generation==latest_.scenery_generation&&
            value.command_id==latest_.command_id&&value.placement_attempt==latest_.placement_attempt&&
            value.target_epoch==latest_.target_epoch&&value.target_producer==latest_.target_producer&&
            value.selection_epoch==latest_.selection_epoch&&value.call_returned_qpc==latest_.call_returned_qpc&&
            (value.flags&~volatile_flags)==(latest_.flags&~volatile_flags);
        if(!forced&&same_core&&now>=latest_.observed_qpc&&
           now-latest_.observed_qpc<static_cast<std::int64_t>(frequency_/4))return;
        if(context.enabled&&sdk_->ready()==Status::ok){
            value.flags|=status::qualified;
            value.flags&=~(status::bindings_ready|status::reference_ready|status::readback_ready);
            if(motion.readiness_bindings()){
                value.flags|=status::bindings_ready;
                if(facts.required&&motion.readiness_reference())value.flags|=status::reference_ready;
                if(facts.required&&motion.readiness_readback())value.flags|=status::readback_ready;
            }
            if(context.lifecycle_pending){value.flags&=~(status::bindings_ready|status::reference_ready|status::readback_ready);
                value.flags|=status::reconciling;}
            if(sdk_generation_!=sdk_->generation())bind();
            if(async_&&sdk_->inspect(async_,xplmType_Int,false)==Status::ok&&
               XPLMGetDataRefTypes(async_)==xplmType_Int&&!XPLMCanWriteDataRef(async_)){
                const auto active=XPLMGetDatai(async_);
                if(active==0||active==1){value.flags|=status::async_available;if(active)value.flags|=status::async_busy;}
            }
        }
        value.reason=(value.flags&(status::placement_active|status::reconciling|status::async_busy))?status::Reason::loading:
            !context.enabled?status::Reason::unsupported:!(value.flags&status::bindings_ready)?status::Reason::binding:
            report.phase==motion::Phase::fault?status::Reason::native_fault:status::Reason::none;
        const bool boundary=latest_.sequence&&(latest_.enable_generation!=value.enable_generation||
            latest_.aircraft_generation!=value.aircraft_generation||latest_.scenery_generation!=value.scenery_generation||
            ((latest_.flags^value.flags)&status::plugin_enabled)||latest_.command_id!=value.command_id||
            latest_.placement_attempt!=value.placement_attempt||latest_.target_source!=value.target_source||
            latest_.target_producer!=value.target_producer||latest_.target_epoch!=value.target_epoch||
            latest_.selection_epoch!=value.selection_epoch);
        if(boundary)++lifecycle_;
        const bool transition=forced||boundary||!latest_.sequence||value.flags!=latest_.flags||value.reason!=latest_.reason||
            value.command_id!=latest_.command_id||value.placement_attempt!=latest_.placement_attempt||
            value.target_epoch!=latest_.target_epoch||value.target_producer!=latest_.target_producer||
            value.selection_epoch!=latest_.selection_epoch;
        if(!transition&&now>=latest_.observed_qpc&&now-latest_.observed_qpc<static_cast<std::int64_t>(frequency_/4))return;
        value.observed_qpc=now;value.sequence=++sequence_;value.lifecycle=lifecycle_;latest_=value;
        if(sink_)sink_(context_,value,transition);
    }catch(...){
        // Optional observer failure never changes motion or the SDK call's behavior.
        const auto now=clock_();auto value=latest_;value.producer=producer_;value.role=status::Role::plugin;
        value.connection_id=1;value.sequence=++sequence_;value.lifecycle=lifecycle_;value.observed_qpc=now;
        value.flags=0;value.reason=status::Reason::native_fault;latest_=value;
        if(sink_)sink_(context_,value,true);
    }
}
}
