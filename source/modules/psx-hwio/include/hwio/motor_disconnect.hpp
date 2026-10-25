#pragma once
#include "button_setup.hpp"

namespace hwio {
// A motor fault requests one aircraft-wide momentary disconnect. This worker-only
// outbox retains unavailable submissions, never replays into a new PSX session,
// and distinguishes an outgoing send from observed native A/T inactivity.
class MotorAtDisconnect {
public:
    enum class Stage { idle, pending, submitted, sent, inactive };
private:
    Stage stage_{Stage::idle};
    unsigned motors_{};
    std::uint64_t epoch_{}, sequence_{}, binding_{}, retry_ms_{}, inactive_sequence_{};
    std::string reason_;
public:
    void boundary(std::uint64_t epoch) {
        stage_=Stage::idle;motors_=0;epoch_=epoch;retry_ms_=0;reason_.clear();
    }
    void request(unsigned motors,std::uint64_t epoch,std::uint64_t inactive_sequence=0) {
        if(!motors)return;
        if(epoch!=epoch_)boundary(epoch);
        if(stage_==Stage::idle || stage_==Stage::inactive) {
            stage_=Stage::pending;motors_=0;retry_ms_=0;reason_.clear();
            binding_=0xF100000000000000ULL | (++sequence_&0x00ffffffffffffffULL);
            inactive_sequence_=inactive_sequence;
        }
        motors_|=motors;
    }
    bool observe_inactive(std::uint64_t epoch,bool valid_inactive,std::uint64_t inactive_sequence=0) {
        // A received inactive-active pair may be drained before this worker
        // runs. Do not send an older queued disconnect after that re-engagement.
        if(epoch==epoch_ && (valid_inactive || (inactive_sequence && inactive_sequence!=inactive_sequence_)) && stage_!=Stage::idle && stage_!=Stage::inactive) {
            stage_=Stage::inactive;reason_.clear();
            return true;
        }
        return false;
    }
    std::optional<Command> next(const Mapper& mapper,const ButtonAction* action,
                                std::uint64_t now,std::int64_t qpc) {
        if(stage_!=Stage::pending || mapper.epoch!=epoch_ || now<retry_ms_)return {};
        retry_ms_=now+250;
        if(!mapper.ready){reason_="PSX connection/baseline unavailable";return {};}
        if(!action){reason_="catalogue A/T-disconnect action missing";return {};}
        const auto& d=action->target;
        // Qh386 contains only the two auto-cleared DELTA switch bits (PSX
        // Network Documentation, ThrustDisc). There is no retained neighbour or
        // startup baseline to preserve. Do not generalize this to packed states.
        if(d.record!=RecordKey{RecordKind::qh,386} || d.layout!=ValueLayout::bit_field ||
           d.semantics!=Semantics::pulse || !d.writable || d.owned_mask!=1 ||
           d.transient_mask!=3 || d.shift || d.minimum!=0 || d.maximum!=1 ||
           action->encoding!=ButtonEncoding::numeric || action->operation!=ButtonOperation::pulse ||
           action->value!=1) {
            reason_="catalogue A/T-disconnect pulse contract unavailable";return {};
        }
        if(const auto unavailable=button_availability(*action,mapper);!unavailable.empty()) {
            reason_=unavailable;return {};
        }
        const auto line=button_compose(*action,"0",action->value);
        if(!line){reason_="catalogue A/T-disconnect pulse composition failed";return {};}
        reason_.clear();return Command{binding_,epoch_,0,qpc,*line+"\n",true};
    }
    bool owns(std::uint64_t binding,std::uint64_t epoch)const noexcept {
        return stage_!=Stage::idle && binding==binding_ && epoch==epoch_;
    }
    void submitted(bool success) {
        if(stage_!=Stage::pending)return;
        if(success){stage_=Stage::submitted;reason_.clear();}
        else reason_="transport submission failed; retry pending";
    }
    bool completed(std::uint64_t binding,std::uint64_t epoch) {
        if(stage_!=Stage::submitted || !owns(binding,epoch))return false;
        stage_=Stage::sent;return true;
    }
    std::uint64_t epoch()const noexcept{return epoch_;}
    std::uint64_t binding()const noexcept{return binding_;}
    unsigned motors()const noexcept{return motors_;}
    Stage stage()const noexcept{return stage_;}
    std::string status()const {
        switch(stage_) {
        case Stage::pending:return "A/T disconnect pending"+(reason_.empty()?std::string{}:": "+reason_);
        case Stage::submitted:return "A/T disconnect submitted; outgoing send pending";
        case Stage::sent:return "A/T disconnect sent; waiting for native A/T inactivity";
        case Stage::inactive:return "Native A/T disconnect observed; re-engagement allowed";
        default:return {};
        }
    }
};
}

