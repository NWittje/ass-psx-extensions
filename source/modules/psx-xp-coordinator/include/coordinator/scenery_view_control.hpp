#pragma once
#include "observation.hpp"
namespace coordinator {
// UI-thread state only. Reconnect queries the enabled plugin lifetime; it never
// restores a saved selection. A queued command is not native-view acceptance.
class SceneryViewControl {
    std::uint64_t connection_{},load_{},enable_{},pending_{};
    std::int64_t sent_{},attempt_{};
    bool available_{},known_{},selected_{},mutation_{};
    psx::bridge::ViewState state_{psx::bridge::ViewState::inactive};
public:
    bool checked()const noexcept{return selected_;}
    bool enabled()const noexcept{return available_&&known_&&(!pending_||!mutation_);}
    const wchar_t* status()const noexcept {
        if(!available_)return L"X-Plane view control unavailable";
        if(pending_&&(mutation_||!known_))return L"Waiting for X-Plane confirmation";
        if(!known_)return L"View state unconfirmed; checking";
        if(state_==psx::bridge::ViewState::unavailable)return L"X-Plane could not enforce this view";
        if(state_==psx::bridge::ViewState::pending)return L"Selected; native view correction pending";
        if(state_==psx::bridge::ViewState::enforcing)return L"Scenery-only view enforced";
        return L"View lock released";
    }
    template<class Request> bool select(bool value,std::int64_t now,Request&& request) {
        if(!enabled())return false;
        const auto id=request(value?psx::bridge::ViewAction::enforce:psx::bridge::ViewAction::release);
        if(!id)return false;
        pending_=id;mutation_=true;sent_=attempt_=now;return true;
    }
    template<class Request> void update(const ObservationSnapshot& view,std::int64_t now,std::int64_t frequency,Request&& request) {
        const auto& p=view.plugin;
        available_=view.bridge==State::connected&&view.scenery_view_available&&view.plugin_available&&
            (p.flags&psx::bridge::enabled)&&p.enable_id&&frequency>0&&now>=view.plugin_received_qpc&&now-view.plugin_received_qpc<frequency;
        if(!available_){pending_=0;known_=false;connection_=load_=enable_=0;attempt_=0;return;}
        if(connection_!=p.connection_id||load_!=p.plugin_load_id||enable_!=p.enable_id){
            connection_=p.connection_id;load_=p.plugin_load_id;enable_=p.enable_id;
            pending_=0;known_=selected_=false;attempt_=0;
        }
        const auto& reply=view.scenery_view;
        if(pending_&&view.scenery_view_reply_available&&reply.operation_id==pending_&&
           reply.connection_id==connection_&&reply.plugin_load_id==load_&&reply.enable_id==enable_){
            pending_=0;known_=reply.reason==psx::bridge::ViewReason::none;
            if(known_){selected_=reply.selected;state_=reply.state;}
        }
        // A missing optional reply is uncertain delivery. Query after the bounded
        // wait, without replaying Enforce/Release or blocking the UI/Follow lane.
        if(pending_&&now>=sent_&&now-sent_>=2*frequency){pending_=0;known_=false;}
        if(!pending_&&(!attempt_||(now>=attempt_&&now-attempt_>=frequency))){
            mutation_=false;attempt_=now;pending_=request(psx::bridge::ViewAction::query);sent_=now;
        }
    }
};
}
