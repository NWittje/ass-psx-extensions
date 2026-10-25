#include "hwio/output_identification.hpp"
#include <algorithm>
#include <atomic>
#include <set>
#include <tuple>

namespace hwio {
namespace {
// Reopening the dialog cannot reuse an old asynchronous operation identity.
std::uint64_t next_session()noexcept{static std::atomic<std::uint64_t> next{1};return next.fetch_add(1,std::memory_order_relaxed);}
auto address(const IdentificationEndpoint& e){return std::tuple{e.resource,e.channel,e.bit};}
bool valid_card(IdentificationCard& c){
    if(c.device.empty() || c.device.size()>256 || !c.connection_generation || !c.resource_generation || c.endpoints.empty() || c.endpoints.size()>1024)return false;
    std::set<std::tuple<std::uint16_t,std::uint16_t,std::uint16_t>> seen;
    for(const auto& e:c.endpoints)if(e.label.size()>160 || !seen.insert(address(e)).second)return false;
    std::sort(c.endpoints.begin(),c.endpoints.end(),[](const auto& a,const auto& b){return std::tuple{a.physical_order,a.resource,a.channel,a.bit}<std::tuple{b.physical_order,b.resource,b.channel,b.bit};});
    return true;
}
}
bool OutputIdentification::gesture(IdentificationGesture g)const noexcept{return g.session==session_ && g.channel==channel_revision_ && g.view==view_revision_;}
bool OutputIdentification::selectable(IdentificationGesture g)const noexcept{return gesture(g) && can_select();}
void OutputIdentification::issue(IdentificationRequestKind kind,std::size_t index){
    pending_=IdentificationRequest{kind,session_,++operation_,card_.connection_generation,card_.resource_generation,index};delivered_=false;
}
void OutputIdentification::root_view(){region_="flightdeck";image_ready_=false;++view_revision_;++channel_revision_;}
bool OutputIdentification::start(IdentificationCard c,bool reset){
    if(!valid_card(c)){reason_="No qualified output inventory";return false;}
    if(!reset){
        if(c.device!=card_.device || c.endpoints.size()!=card_.endpoints.size()){reason_="Output inventory changed; retain the draft and review the device";return false;}
        for(std::size_t n=0;n<c.endpoints.size();++n)if(address(c.endpoints[n])!=address(card_.endpoints[n])){reason_="Output addresses changed; retain the draft and review the device";return false;}
    }
    card_=std::move(c);announced_connection_=card_.connection_generation;announced_resources_=card_.resource_generation;session_=next_session();edit_.reset();undo_after_clear_=review_after_edit_=false;reason_.clear();
    if(reset){choices_.assign(card_.endpoints.size(),{});history_.clear();history_.reserve(2*card_.endpoints.size());channel_=0;retry_only_=false;}
    root_view();phase_=IdentificationPhase::acquiring;issue(IdentificationRequestKind::acquire);return true;
}
bool OutputIdentification::begin(IdentificationCard c){if(phase_!=IdentificationPhase::idle && phase_!=IdentificationPhase::closed)return false;return start(std::move(c),true);}
bool OutputIdentification::resume(IdentificationCard c){if(phase_!=IdentificationPhase::paused || owned_ || pending_)return false;return start(std::move(c),false);}
std::optional<IdentificationRequest> OutputIdentification::take_request(){if(!pending_ || delivered_)return {};delivered_=true;return pending_;}
void OutputIdentification::release(IdentificationPhase goal){after_release_=goal;phase_=IdentificationPhase::releasing;image_ready_=false;++view_revision_;issue(IdentificationRequestKind::release);}
void OutputIdentification::fail(std::string why){
    reason_=std::move(why);edit_.reset();undo_after_clear_=false;image_ready_=false;++view_revision_;
    if(phase_==IdentificationPhase::releasing){if(after_release_!=IdentificationPhase::closed)after_release_=IdentificationPhase::paused;return;}
    if(owned_ || (pending_ && pending_->kind==IdentificationRequestKind::acquire))release(IdentificationPhase::paused);
    else {pending_.reset();phase_=IdentificationPhase::paused;}
}
void OutputIdentification::illuminate(){
    if(channel_>=choices_.size()){release(IdentificationPhase::review);return;}
    phase_=IdentificationPhase::lighting;issue(IdentificationRequestKind::on,channel_);
}
void OutputIdentification::apply_undo(){
    const auto h=history_.back();history_.pop_back();channel_=h.channel;choices_[channel_]=h.previous;
    retry_only_=h.previous.kind!=IdentificationChoiceKind::unvisited;root_view();
}
void OutputIdentification::change_after_off(){
    const auto edit=std::move(*edit_);edit_.reset();
    if(edit.kind==EditKind::undo){apply_undo();illuminate();return;}
    const IdentificationChoice next{edit.kind==EditKind::assign?IdentificationChoiceKind::assigned:IdentificationChoiceKind::skipped,edit.output};
    if(next!=choices_[channel_]){history_.push_back({channel_,choices_[channel_]});choices_[channel_]=next;}
    if(review_after_edit_){review_after_edit_=false;release(IdentificationPhase::review);return;}
    if(retry_only_){channel_=choices_.size();release(IdentificationPhase::review);return;}
    do {++channel_;}while(channel_<choices_.size() && choices_[channel_].kind!=IdentificationChoiceKind::unvisited);
    root_view();illuminate();
}
bool OutputIdentification::complete(const IdentificationCompletion& c){
    if(!pending_)return false;const auto expected=*pending_;const auto& r=c.request;
    if(r.kind!=expected.kind || r.session!=expected.session || r.operation!=expected.operation || r.connection_generation!=expected.connection_generation || r.resource_generation!=expected.resource_generation || r.channel!=expected.channel)return false;
    // An unsuccessful release must never be represented as confirmed restoration.
    if(r.kind==IdentificationRequestKind::release){
        pending_.reset();owned_=!c.ownership_released;
        if(c.success && c.ownership_released){phase_=after_release_;return true;}
        phase_=after_release_==IdentificationPhase::closed && c.ownership_released?IdentificationPhase::closed:IdentificationPhase::paused;
        reason_=c.reason.empty()?"Output restoration was not confirmed":c.reason;return true;
    }
    if(!c.success){
        if(c.ownership_released){pending_.reset();owned_=false;reason_=c.reason;edit_.reset();phase_=IdentificationPhase::paused;image_ready_=false;++view_revision_;}
        else fail(c.reason.empty()?"Output identification failed":c.reason);
        return true;
    }
    pending_.reset();
    switch(r.kind){
    case IdentificationRequestKind::acquire:owned_=true;phase_=IdentificationPhase::clearing;issue(IdentificationRequestKind::all_off);break;
    case IdentificationRequestKind::all_off:if(undo_after_clear_){undo_after_clear_=false;apply_undo();}illuminate();break;
    case IdentificationRequestKind::on:phase_=IdentificationPhase::selecting;break;
    case IdentificationRequestKind::off:change_after_off();break;
    case IdentificationRequestKind::release:break;
    }
    return true;
}
bool OutputIdentification::image_ready(IdentificationGesture g)noexcept{
    if(!gesture(g) || (phase_!=IdentificationPhase::acquiring && phase_!=IdentificationPhase::clearing && phase_!=IdentificationPhase::lighting && phase_!=IdentificationPhase::selecting))return false;
    image_ready_=true;return true;
}
bool OutputIdentification::navigate(std::string_view region,IdentificationGesture g){
    if(!selectable(g) || region.empty() || region.size()>128)return false;region_=region;image_ready_=false;++view_revision_;return true;
}
bool OutputIdentification::choose(std::string_view output,bool qualified,IdentificationGesture g){
    if(!selectable(g) || !qualified || !output.starts_with("out.") || output.size()>128 || output.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-")!=output.npos)return false;
    edit_=Edit{EditKind::assign,std::string(output)};phase_=IdentificationPhase::channel_off;image_ready_=false;++view_revision_;issue(IdentificationRequestKind::off,channel_);return true;
}
bool OutputIdentification::skip(IdentificationGesture g){
    if(!selectable(g) || region_!="flightdeck")return false;
    edit_=Edit{EditKind::skip,{}};phase_=IdentificationPhase::channel_off;image_ready_=false;++view_revision_;issue(IdentificationRequestKind::off,channel_);return true;
}
bool OutputIdentification::undo(IdentificationGesture g){
    if(!gesture(g) || history_.empty())return false;
    if(phase_==IdentificationPhase::selecting){edit_=Edit{EditKind::undo,{}};phase_=IdentificationPhase::channel_off;image_ready_=false;++view_revision_;issue(IdentificationRequestKind::off,channel_);return true;}
    if(phase_!=IdentificationPhase::review || owned_)return false;
    session_=next_session();root_view();undo_after_clear_=true;phase_=IdentificationPhase::acquiring;issue(IdentificationRequestKind::acquire);return true;
}
bool OutputIdentification::retry_skipped(std::size_t index,IdentificationGesture g){
    if(!gesture(g) || phase_!=IdentificationPhase::review || owned_ || index>=choices_.size() || choices_[index].kind!=IdentificationChoiceKind::skipped)return false;
    channel_=index;retry_only_=true;session_=next_session();root_view();phase_=IdentificationPhase::acquiring;issue(IdentificationRequestKind::acquire);return true;
}
void OutputIdentification::inventory_changed(std::uint64_t connection,std::uint64_t resources){
    if(connection==announced_connection_ && resources==announced_resources_)return;
    announced_connection_=connection;announced_resources_=resources;
    if(phase_!=IdentificationPhase::idle && phase_!=IdentificationPhase::closed)fail("Output connection or resource generation changed; draft retained");
}
void OutputIdentification::pause(std::string reason){
    if(phase_==IdentificationPhase::idle || phase_==IdentificationPhase::closed)return;
    if(reason.size()>512)reason.resize(512);fail(std::move(reason));
}
void OutputIdentification::review(){
    if(phase_==IdentificationPhase::idle || phase_==IdentificationPhase::closed || phase_==IdentificationPhase::review)return;
    // A click already accepted by the UI remains a draft only after its channel
    // OFF acknowledgement. Finish waits for that transaction before restoring.
    if(phase_==IdentificationPhase::channel_off && edit_){review_after_edit_=true;return;}
    edit_.reset();undo_after_clear_=false;
    if(phase_==IdentificationPhase::releasing){after_release_=IdentificationPhase::review;return;}
    if(owned_ || pending_)release(IdentificationPhase::review);
    else {phase_=IdentificationPhase::review;image_ready_=false;++view_revision_;}
}
void OutputIdentification::stop(){
    edit_.reset();undo_after_clear_=review_after_edit_=false;
    if(phase_==IdentificationPhase::releasing){after_release_=IdentificationPhase::closed;return;}
    if(owned_ || pending_)release(IdentificationPhase::closed);
    else {phase_=IdentificationPhase::closed;image_ready_=false;++view_revision_;}
}
bool OutputIdentification::finish(bool persisted,std::string_view error){
    if(phase_!=IdentificationPhase::review || owned_ || pending_)return false;
    if(!persisted){reason_=error;return false;}phase_=IdentificationPhase::closed;++view_revision_;return true;
}
}
