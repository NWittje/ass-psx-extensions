#include "psx/hardware/host_session.hpp"
#include <algorithm>
#include <limits>

namespace psx::hardware {
void HostSession::start(HostBinding binding,HostCallbacks callbacks,std::uint64_t challenge,
                        std::uint32_t session,std::uint64_t now,PublicationPolicy policy) noexcept {
    *this=HostSession{};binding_=binding;callbacks_=callbacks;policy_=policy;
    challenge_=challenge?challenge:1;status_.session=session?session:1;
    overall_deadline_=now+12000000;discover(now);
}
void HostSession::discover(std::uint64_t now) noexcept {
    status_.phase=HostPhase::discovering;status_.identified=false;
    hello_sent_=false;
    status_.has_device_sequence=false;status_.has_sent=false;
    phase_deadline_=now+5000000;control_due_=now;
    // Separate boot garbage from the first host frame. Never overwrite an active transport buffer.
    if(pending_tx().empty()){tx_={};tx_.data[0]=0;tx_.size=1;tx_offset_=0;}
}
void HostSession::fail(HostFault fault) noexcept {status_.fault=fault;status_.phase=HostPhase::failed;}
void HostSession::transport_fault() noexcept {fail(HostFault::transport);}
void HostSession::close() noexcept {*this=HostSession{};}
Bytes HostSession::pending_tx() const noexcept {return Bytes(tx_.data.data()+tx_offset_,tx_.size-tx_offset_);}
bool HostSession::consume_tx(std::size_t n) noexcept {
    if(n>tx_.size-tx_offset_)return false;
    tx_offset_+=n;return true;
}
bool HostSession::queue(Type type,Bytes payload,std::uint32_t session,std::uint32_t sequence) noexcept {
    if(!pending_tx().empty())return false;
    tx_offset_=0;return encode({std::uint8_t(type),session,sequence,payload},tx_);
}
SendResult HostSession::publish_latest(const ProfileState& state) noexcept {
    if(state.type<64||state.size>max_payload)return SendResult::invalid;
    if(status_.phase==HostPhase::closed||status_.phase==HostPhase::failed)return SendResult::busy;
    if(!has_latest_||state.type!=latest_.type||state.size!=latest_.size||
       !std::equal(state.payload.begin(),state.payload.begin()+state.size,latest_.payload.begin()))dirty_=true;
    latest_=state;has_latest_=true;return SendResult::accepted;
}
void HostSession::service(std::uint64_t now) noexcept {
    if(status_.phase==HostPhase::closed||status_.phase==HostPhase::failed)return;
    if(status_.phase!=HostPhase::ready&&now>=overall_deadline_){fail(HostFault::handshake_timeout);return;}
    if(status_.phase==HostPhase::discovering&&now>=phase_deadline_){fail(HostFault::discovery_timeout);return;}
    if(status_.phase==HostPhase::starting&&now>=phase_deadline_){
        ++challenge_;if(!challenge_)++challenge_;
        ++status_.session;if(!status_.session)++status_.session;
        discover(now);
    }
    if(status_.phase==HostPhase::ready&&now-last_rx_>=policy_.peer_timeout_us){fail(HostFault::peer_timeout);return;}
    if(!pending_tx().empty())return;
    if(status_.phase==HostPhase::discovering&&now>=control_due_){
        if(hello_sent_){++challenge_;if(!challenge_)++challenge_;}
        hello_sent_=true;
        std::array<std::uint8_t,8> payload{};write64(payload,0,challenge_);
        queue(Type::hello,payload,0,0);control_due_=now+500000;
    }else if(status_.phase==HostPhase::starting&&now>=control_due_){
        std::array<std::uint8_t,28> payload{};write64(payload,0,challenge_);
        write64(payload,8,status_.identity.device);write64(payload,16,status_.identity.boot);
        write16(payload,24,binding_.profile);write16(payload,26,binding_.revision);
        queue(Type::start_session,payload,status_.session,0);control_due_=now+250000;
    }else if(status_.phase==HostPhase::ready&&has_latest_&&now>=send_due_&&
             (dirty_||!status_.has_sent||now-last_send_>=policy_.heartbeat_us)){
        if(queue(static_cast<Type>(latest_.type),Bytes(latest_.payload.data(),latest_.size),status_.session,sequence_)){
            status_.last_sent=sequence_++;status_.has_sent=true;dirty_=false;
            last_send_=now;send_due_=now+policy_.minimum_interval_us;
        }
    }
}
void HostSession::receive(Bytes input,std::uint64_t now) noexcept {
    if(status_.phase==HostPhase::closed||status_.phase==HostPhase::failed)return;
    for(auto byte:input){
        if(!byte){
            if(!discard_&&rx_size_){Message message{};
                if(decode(Bytes(rx_.data(),rx_size_),decoded_,message))dispatch(message,now);
                else ++status_.rejected;
            }
            rx_size_=0;discard_=false;
        }else if(!discard_){
            if(rx_size_==rx_.size()){discard_=true;rx_size_=0;++status_.oversized;}
            else rx_[rx_size_++]=byte;
        }
    }
}
void HostSession::dispatch(const Message& message,std::uint64_t now) noexcept {
    const auto payload=message.payload;
    if(status_.phase==HostPhase::discovering&&message.type==std::uint8_t(Type::hello_reply)&&message.session==0){
        if(payload.size()<35||read64(payload,0)!=challenge_){++status_.rejected;return;}
        const auto size=payload[34];
        if(size>48||payload.size()!=std::size_t(35+size)){++status_.rejected;return;}
        Descriptor descriptor{};descriptor.device=read64(payload,8);descriptor.boot=read64(payload,16);
        descriptor.firmware=read32(payload,24);descriptor.board=read16(payload,28);
        descriptor.profile=read16(payload,30);descriptor.profile_revision=read16(payload,32);
        descriptor.metadata_size=size;std::copy(payload.begin()+35,payload.end(),descriptor.metadata.begin());
        if(!descriptor.device||!descriptor.boot||descriptor.board!=binding_.board||descriptor.profile!=binding_.profile||
           descriptor.profile_revision!=binding_.revision||(binding_.expected_device&&descriptor.device!=binding_.expected_device)||
           (callbacks_.identify&&!callbacks_.identify(callbacks_.context,descriptor))){fail(HostFault::identity_mismatch);return;}
        status_.identity=descriptor;status_.identified=true;
        // Pin the identity for any repeated discovery during this explicit open.
        binding_.expected_device=descriptor.device;status_.phase=HostPhase::starting;
        phase_deadline_=now+2000000;control_due_=now;return;
    }
    if(status_.phase==HostPhase::starting&&message.type==std::uint8_t(Type::ready)&&
       message.session==status_.session&&payload.size()==8&&read64(payload,0)==challenge_){
        status_.phase=HostPhase::ready;status_.last_device=message.sequence;status_.has_device_sequence=true;
        last_rx_=now;sequence_=1;send_due_=now;dirty_=has_latest_;return;
    }
    if(status_.phase==HostPhase::ready&&message.session==status_.session&&
       (message.type==std::uint8_t(Type::status)||message.type>=64)&&
       (!status_.has_device_sequence||newer(message.sequence,status_.last_device))&&
       callbacks_.message&&callbacks_.message(callbacks_.context,message,now)){
        status_.last_device=message.sequence;status_.has_device_sequence=true;last_rx_=now;return;
    }
    ++status_.rejected;
}
std::uint64_t HostSession::next_wake(std::uint64_t now) const noexcept {
    auto due=now+50000;
    if(status_.phase==HostPhase::discovering||status_.phase==HostPhase::starting)
        due=std::min({due,control_due_,phase_deadline_,overall_deadline_});
    if(status_.phase==HostPhase::ready){
        due=std::min(due,last_rx_+policy_.peer_timeout_us);
        if(has_latest_)due=std::min(due,dirty_?send_due_:std::max(send_due_,last_send_+policy_.heartbeat_us));
    }
    return std::max(now,due);
}
}
