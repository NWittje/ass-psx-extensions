#include "psx_plugin/scenery_view_controller.hpp"

namespace psx::xp {
namespace {
// These change the native mode or the aircraft-relative look direction. Projector/FOV,
// eye translations, lighting, camera effects and memorising a Quick Look are untouched.
constexpr const char* incompatible[]{
    "sim/view/default_view","sim/view/forward_with_2d_panel","sim/view/forward_with_hud",
    "sim/view/free_camera","sim/view/linear_spot","sim/view/still_spot","sim/view/runway",
    "sim/view/circle","sim/view/tower","sim/view/ridealong","sim/view/track_weapon",
    "sim/view/chase","sim/view/wingman","sim/view/3d_cockpit_cmnd_look","sim/view/3d_cockpit_toggle",
    "sim/view/glance_left","sim/view/glance_right","sim/view/ai_controls_views",
    "sim/view/rot_cockpit_left_45","sim/view/rot_cockpit_right_45","sim/view/rot_cockpit_up_30","sim/view/rot_cockpit_down_30",
    "sim/general/rot_left","sim/general/rot_right","sim/general/rot_up","sim/general/rot_down",
    "sim/general/rot_left_fast","sim/general/rot_right_fast","sim/general/rot_up_fast","sim/general/rot_down_fast",
    "sim/general/rot_left_slow","sim/general/rot_right_slow","sim/general/rot_up_slow","sim/general/rot_down_slow",
    "sim/view/quick_look_0","sim/view/quick_look_1","sim/view/quick_look_2","sim/view/quick_look_3",
    "sim/view/quick_look_4","sim/view/quick_look_5","sim/view/quick_look_6","sim/view/quick_look_7",
    "sim/view/quick_look_8","sim/view/quick_look_9","sim/view/quick_look_10","sim/view/quick_look_11",
    "sim/view/quick_look_12","sim/view/quick_look_13","sim/view/quick_look_14","sim/view/quick_look_15",
    "sim/view/quick_look_16","sim/view/quick_look_17","sim/view/quick_look_18","sim/view/quick_look_19"
};
static_assert(std::size(incompatible)<=64);
}
void SceneryViewController::begin(SdkApi& sdk,std::int64_t (*clock)() noexcept,std::uint64_t frequency) noexcept {
    if(sdk.ready()!=Status::ok)return;
    stop();sdk_=&sdk;clock_=clock;frequency_=frequency;state_=SceneryViewState::released;
    try {
        target_=XPLMFindCommand("sim/view/forward_with_nothing");
        if(!target_||!clock_||!frequency_){state_=SceneryViewState::unavailable;return;}
        XPLMRegisterCommandHandler(target_,observed,0,this);hooked_=true;
        for(const auto* name:incompatible)if(auto command=XPLMFindCommand(name)) {
            XPLMRegisterCommandHandler(command,block,1,this);blocked_[blocked_count_++]=command;
        }
        bind();
    }catch(...){stop();state_=SceneryViewState::unavailable;}
}
bool SceneryViewController::bind() noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok)return false;
    try {
        view_=XPLMFindDataRef("sim/graphics/view/view_type");generation_=sdk_->generation();
        sampled_=attempted_=false;failures_=0;
        if(sdk_->inspect(view_,xplmType_Int,false)==Status::ok)return true;
    }catch(...){}
    view_=nullptr;state_=SceneryViewState::unavailable;return false;
}
void SceneryViewController::stop() noexcept {
    if(sdk_&&!sdk_->on_thread())return;
    selected_=false;
    try {
        if(hooked_)XPLMUnregisterCommandHandler(target_,observed,0,this);
        for(std::size_t i=0;i<blocked_count_;++i)XPLMUnregisterCommandHandler(blocked_[i],block,1,this);
    }catch(...){}
    sdk_=nullptr;clock_=nullptr;frequency_=generation_=0;target_=nullptr;view_=nullptr;
    blocked_count_=0;hooked_=sampled_=attempted_=false;failures_=0;expected_=observed_=0;
    state_=SceneryViewState::inactive;
    last_request_={};last_reason_=bridge::ViewReason::none;connection_=0;
}
void SceneryViewController::select(bool value) noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok||selected_==value)return;
    selected_=value;sampled_=attempted_=false;failures_=0;
    state_=value?SceneryViewState::pending:SceneryViewState::released;
}
int SceneryViewController::block(XPLMCommandRef,XPLMCommandPhase,void* context) noexcept {
    const auto& self=*static_cast<SceneryViewController*>(context);
    return self.sdk_&&self.sdk_->ready()==Status::ok&&self.selected_&&self.state_!=SceneryViewState::unavailable?0:1;
}
int SceneryViewController::observed(XPLMCommandRef,XPLMCommandPhase phase,void* context) noexcept {
    auto& self=*static_cast<SceneryViewController*>(context);
    if(!self.sdk_||self.sdk_->ready()!=Status::ok||!self.selected_||!self.view_||phase!=xplm_CommandBegin)return 1;
    try {
        // Learn the mode only after X-Plane processed the named native command.
        // No guessed enum and no baseline sampled from an unrelated cockpit mode.
        const auto mode=XPLMGetDatai(self.view_);
        if(mode>0){self.expected_=self.observed_=mode;self.sampled_=true;}
    }catch(...){self.sampled_=false;}
    return 1;
}
void SceneryViewController::tick() noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok||!selected_)return;
    try {
        if(generation_!=sdk_->generation()&&!bind())return;
        if(!target_||!view_||!hooked_){state_=SceneryViewState::unavailable;return;}
        observed_=XPLMGetDatai(view_);
        if(sampled_&&observed_==expected_){state_=SceneryViewState::enforcing;failures_=0;return;}
        state_=SceneryViewState::pending;
        const auto now=clock_();
        if(now<=0||failures_>=3){state_=SceneryViewState::unavailable;return;}
        // At most one correction per second even if a competing camera changes every frame.
        if(attempted_&&now>=last_attempt_&&static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(last_attempt_)<frequency_)return;
        attempted_=true;last_attempt_=now;sampled_=false;
        XPLMCommandOnce(target_);
        observed_=XPLMGetDatai(view_);
        if(sampled_&&observed_==expected_){state_=SceneryViewState::enforcing;failures_=0;}
        else if(++failures_>=3)state_=SceneryViewState::unavailable;
    }catch(...){state_=SceneryViewState::unavailable;failures_=3;}
}
bridge::ViewReply SceneryViewController::request(const bridge::ViewRequest& request,std::uint64_t connection,std::uint64_t load,std::uint64_t enable) noexcept {
    if(!sdk_||sdk_->ready()!=Status::ok)return {connection,request.operation_id,load,enable,0,bridge::ViewState::inactive,bridge::ViewReason::identity,false,0};
    if(connection_!=connection){connection_=connection;last_request_={};last_reason_=bridge::ViewReason::none;}
    const auto now=clock_?clock_():0;
    auto reason=bridge::ViewReason::none;
    if(!sdk_||sdk_->ready()!=Status::ok||!connection||request.connection_id!=connection||request.plugin_load_id!=load||request.enable_id!=enable||
       !request.operation_id||static_cast<unsigned>(request.action)>2||request.operation_id<last_request_.operation_id||
       (request.operation_id==last_request_.operation_id&&request!=last_request_))reason=bridge::ViewReason::identity;
    else if(request.operation_id==last_request_.operation_id)reason=last_reason_;
    else {
        last_request_=request;
        if(request.requested_qpc<0||request.requested_qpc>now||static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(request.requested_qpc)>=frequency_)reason=bridge::ViewReason::expired;
        else if(request.action!=bridge::ViewAction::query){select(request.action==bridge::ViewAction::enforce);tick();}
        last_reason_=reason;
    }
    try {
        if(generation_!=sdk_->generation())bind();
        if(view_)observed_=XPLMGetDatai(view_);
    }catch(...){state_=SceneryViewState::unavailable;}
    if(reason==bridge::ViewReason::none&&state_==SceneryViewState::unavailable)reason=bridge::ViewReason::binding;
    bridge::ViewReply reply{connection,request.operation_id,load,enable,clock_?clock_():0,state_,reason,selected_,static_cast<std::uint32_t>(observed_>0?observed_:0)};
    return reply;
}
}
