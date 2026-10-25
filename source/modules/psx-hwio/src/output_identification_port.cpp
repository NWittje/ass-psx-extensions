#include "hwio/output_identification_port.hpp"
#include <algorithm>
#include <tuple>

namespace hwio {
bool IdentificationPort::submit(IdentificationRequest request,const IdentificationCard& card){
    if(!request.session || !request.operation)return false;
    if(request.kind==IdentificationRequestKind::acquire && (card.device.empty() || card.device.size()>256 || card.endpoints.empty() || card.endpoints.size()>1024 || !card.connection_generation || !card.resource_generation || std::any_of(card.endpoints.begin(),card.endpoints.end(),[](const auto& e){return e.label.size()>160;})))return false;
    std::lock_guard lock(mutex_);
    // Release supersedes a command not yet consumed. It cannot interrupt a native
    // call already running; the worker acknowledges that call before restoring.
    if(command_ && request.kind!=IdentificationRequestKind::release)return false;
    if(command_ && command_->request.session!=request.session)return false;
    command_=Command{request,request.kind==IdentificationRequestKind::acquire?card:IdentificationCard{}};
    return true;
}
std::optional<IdentificationPort::Command> IdentificationPort::take(){std::lock_guard lock(mutex_);auto c=std::move(command_);command_.reset();return c;}
void IdentificationPort::complete(IdentificationCompletion c){
    std::lock_guard lock(mutex_);
    // At most one request is outstanding per session. Closed-dialog replies are
    // dispensable; retaining them must not grow with repeated dialog openings.
    if(replies_.size()==8)replies_.pop_front();replies_.push_back(std::move(c));
}
std::optional<IdentificationCompletion> IdentificationPort::poll(std::uint64_t session){
    std::lock_guard lock(mutex_);
    const auto i=std::find_if(replies_.begin(),replies_.end(),[&](const auto& c){return c.request.session==session;});
    if(i==replies_.end())return {};auto c=std::move(*i);replies_.erase(i);return c;
}
bool IdentificationLease::owns(std::uint16_t resource,std::uint16_t channel,std::uint16_t bit)const noexcept{
    if(!active())return false;
    for(std::size_t n=0;n<normal_.size();++n)if(normal_[n].owned && card_.endpoints[n].resource==resource && card_.endpoints[n].channel==channel && card_.endpoints[n].bit==bit)return true;
    return false;
}
bool IdentificationLease::normal(std::uint16_t resource,std::uint16_t channel,bool on,std::uint16_t bit)noexcept{
    if(!active())return false;
    for(std::size_t n=0;n<normal_.size();++n)if(card_.endpoints[n].resource==resource && card_.endpoints[n].channel==channel && card_.endpoints[n].bit==bit){
        auto& v=normal_[n];if(v.on!=on){v.on=on;++v.revision;}return v.owned;
    }
    return false;
}
void IdentificationLease::acquire(IdentificationPort& port,IdentificationPort::Command c,std::span<const std::uint8_t> initial,std::string error){
    if(active())error="Another output-identification session owns this card";
    if(c.request.kind!=IdentificationRequestKind::acquire || c.card.endpoints.empty() || c.card.endpoints.size()>1024 || initial.size()!=c.card.endpoints.size() || c.request.connection_generation!=c.card.connection_generation || c.request.resource_generation!=c.card.resource_generation)error="Invalid output-identification inventory";
    std::vector<std::tuple<std::uint16_t,std::uint16_t,std::uint16_t>> addresses;addresses.reserve(c.card.endpoints.size());
    for(const auto& e:c.card.endpoints)addresses.emplace_back(e.resource,e.channel,e.bit);
    std::sort(addresses.begin(),addresses.end());if(std::adjacent_find(addresses.begin(),addresses.end())!=addresses.end())error="Duplicate output-identification address";
    if(!error.empty()){port.complete({c.request,false,true,std::move(error)});return;}
    card_=std::move(c.card);port_=&port;normal_.assign(initial.size(),{});
    for(std::size_t n=0;n<initial.size();++n)normal_[n].on=initial[n];
    operation_.reset();writing_.reset();cursor_=0;session_=c.request.session;last_operation_=c.request.operation;cleared_=false;illuminated_.reset();
    // Remember the acquisition identity even between operations.
    operation_=c.request;reply(true,false);
}
void IdentificationLease::reply(bool success,bool released,std::string reason){
    if(operation_ && port_)port_->complete({*operation_,success,released,std::move(reason)});
    operation_.reset();
}
void IdentificationLease::request(IdentificationRequest r){
    if(!active())return;
    if(r.session!=session_ || r.operation<=last_operation_){port_->complete({r,false,false,"Stale output-identification operation"});return;}
    if(r.connection_generation!=card_.connection_generation || r.resource_generation!=card_.resource_generation){port_->complete({r,false,false,"Output generation changed"});return;}
    if(operation_ && r.kind!=IdentificationRequestKind::release){port_->complete({r,false,false,"Output operation is still pending"});return;}
    if(r.kind==IdentificationRequestKind::acquire || ((r.kind==IdentificationRequestKind::on || r.kind==IdentificationRequestKind::off) && r.channel>=normal_.size())){port_->complete({r,false,false,"Invalid output operation"});return;}
    if(r.kind==IdentificationRequestKind::on && (!cleared_ || (illuminated_ && *illuminated_!=r.channel))){port_->complete({r,false,false,"Clear the card and release the previous channel before illuminating another"});return;}
    operation_=r;last_operation_=r.operation;cursor_=0;
}
std::optional<IdentificationLease::Write> IdentificationLease::next(){
    if(!active() || !operation_ || writing_)return {};
    const auto kind=operation_->kind;
    if(kind==IdentificationRequestKind::release){
        while(cursor_<normal_.size() && !normal_[cursor_].owned)++cursor_;
        if(cursor_==normal_.size()){finish_release();return {};}
    }else if(kind==IdentificationRequestKind::all_off){
        if(cursor_==normal_.size()){cleared_=true;illuminated_.reset();reply(true,false);return {};}
    }else cursor_=operation_->channel;
    const bool on=kind==IdentificationRequestKind::on || (kind==IdentificationRequestKind::release && normal_[cursor_].on);
    writing_revision_=normal_[cursor_].revision;writing_cursor_=cursor_;writing_restore_=kind==IdentificationRequestKind::release;
    writing_=Write{card_.endpoints[cursor_],on,++next_ticket_};return writing_;
}
void IdentificationLease::complete(std::uint64_t ticket,bool success,std::string reason){
    if(!writing_ || writing_->ticket!=ticket)return;
    writing_.reset();
    if(!operation_)return;
    if(!success){
        // Leave ownership intact. An explicit release may still restore other
        // endpoints; the failing write is never acknowledged as illumination.
        cleared_=false;reply(false,false,reason.empty()?"Output write failed":std::move(reason));return;
    }
    // Release may have superseded the operation while its write was in flight.
    // Such a write did not restore normal intent and cannot release ownership.
    if(operation_->kind==IdentificationRequestKind::release){
        if(writing_restore_ && writing_cursor_==cursor_ && normal_[cursor_].revision==writing_revision_){normal_[cursor_].owned=false;++cursor_;}
    }else if(operation_->kind==IdentificationRequestKind::all_off)++cursor_;
    else {if(operation_->kind==IdentificationRequestKind::on)illuminated_=cursor_;else illuminated_.reset();reply(true,false);}
}
void IdentificationLease::finish_release(){reply(true,true);port_=nullptr;normal_.clear();card_={};}
void IdentificationLease::lost(std::string reason){if(!active())return;if(!operation_)operation_=IdentificationRequest{IdentificationRequestKind::release,session_,++last_operation_,card_.connection_generation,card_.resource_generation};reply(false,true,std::move(reason));port_=nullptr;writing_.reset();normal_.clear();card_={};}
void IdentificationLease::release(){
    if(!active())return;
    // Provider shutdown uses the last session identity; a UI release, when
    // present, remains the operation whose completion must be delivered.
    if(operation_ && operation_->kind==IdentificationRequestKind::release)return;
    operation_=IdentificationRequest{IdentificationRequestKind::release,session_,++last_operation_,card_.connection_generation,card_.resource_generation};cursor_=0;
}
}
