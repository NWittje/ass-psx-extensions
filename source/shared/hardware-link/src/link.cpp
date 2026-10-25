#include "psx/hardware/link.hpp"
#include <algorithm>

namespace psx::hardware {
std::uint16_t read16(Bytes b,std::size_t p) noexcept { return std::uint16_t(b[p]) | std::uint16_t(std::uint16_t(b[p+1])<<8); }
std::uint32_t read32(Bytes b,std::size_t p) noexcept { return std::uint32_t(read16(b,p)) | (std::uint32_t(read16(b,p+2))<<16); }
std::uint64_t read64(Bytes b,std::size_t p) noexcept { return std::uint64_t(read32(b,p)) | (std::uint64_t(read32(b,p+4))<<32); }
void write16(std::span<std::uint8_t> b,std::size_t p,std::uint16_t n) noexcept { b[p]=std::uint8_t(n); b[p+1]=std::uint8_t(n>>8); }
void write32(std::span<std::uint8_t> b,std::size_t p,std::uint32_t n) noexcept { write16(b,p,std::uint16_t(n)); write16(b,p+2,std::uint16_t(n>>16)); }
void write64(std::span<std::uint8_t> b,std::size_t p,std::uint64_t n) noexcept { write32(b,p,std::uint32_t(n)); write32(b,p+4,std::uint32_t(n>>32)); }
std::uint16_t crc16(Bytes bytes) noexcept {
    std::uint16_t crc=0xffff;
    for(auto b:bytes){crc=std::uint16_t(crc ^ (std::uint16_t(b)<<8));for(int i=0;i<8;++i) crc=std::uint16_t((crc&0x8000u)?(crc<<1)^0x1021u:crc<<1);}
    return crc;
}
bool newer(std::uint32_t a,std::uint32_t b) noexcept { const auto d=std::uint32_t(a-b);return d!=0 && d<0x80000000u; }
bool encode(const Message& m,Frame& out) noexcept {
    out.size=0;
    if(m.payload.size()>max_payload)return false;
    std::array<std::uint8_t,max_wire-2> raw{};
    raw[0]='P';raw[1]='X';raw[2]='T';raw[3]='1';raw[4]=1;raw[5]=m.type;
    write16(raw,6,std::uint16_t(m.payload.size()));write32(raw,8,m.session);write32(raw,12,m.sequence);
    std::copy(m.payload.begin(),m.payload.end(),raw.begin()+header_size);
    const auto n=header_size+m.payload.size();write16(raw,n,crc16(Bytes(raw.data(),n)));
    std::size_t code_at=0, pos=1;std::uint8_t code=1;
    for(std::size_t i=0;i<n+2;++i){if(raw[i]==0){out.data[code_at]=code;code_at=pos++;code=1;}else{out.data[pos++]=raw[i];++code;}}
    out.data[code_at]=code;out.data[pos++]=0;out.size=pos;return true;
}
bool decode(Bytes encoded,std::span<std::uint8_t> raw,Message& out) noexcept {
    if(encoded.empty()||encoded.size()>max_wire-1)return false;
    std::size_t p=0,n=0;
    while(p<encoded.size()){
        auto code=encoded[p++];if(code==0 || std::size_t(code-1)>encoded.size()-p)return false;
        for(unsigned i=1;i<code;++i){if(n>=raw.size()||encoded[p]==0)return false;raw[n++]=encoded[p++];}
        if(code!=255 && p<encoded.size()){if(n>=raw.size())return false;raw[n++]=0;}
    }
    if(n<header_size+2 || raw[0]!='P'||raw[1]!='X'||raw[2]!='T'||raw[3]!='1'||raw[4]!=1)return false;
    const Bytes b(raw.data(),n);const auto len=read16(b,6);
    if(len>max_payload || n!=header_size+len+2 || read16(b,n-2)!=crc16(b.first(n-2)))return false;
    out={raw[5],read32(b,8),read32(b,12),b.subspan(header_size,len)};return true;
}
HardwareLink::HardwareLink(Descriptor d,Callbacks c) noexcept:descriptor_(d),callbacks_(c) {
    if(descriptor_.metadata_size>descriptor_.metadata.size())descriptor_.metadata_size=0;
}
bool HardwareLink::enqueue(std::uint8_t type,std::uint32_t session,Bytes payload) noexcept {
    if(count_==queue_.size()){++status_.tx_busy;return false;}
    if(!encode({type,session,next_tx_,payload},queue_[(head_+count_)%queue_.size()]))return false;
    ++next_tx_;++count_;return true;
}
SendResult HardwareLink::try_send(std::uint8_t type,Bytes payload) noexcept {
    if(!status_.active || (type!=std::uint8_t(Type::status)&&type<64) || payload.size()>max_payload)return SendResult::invalid;
    return enqueue(type,status_.session,payload)?SendResult::accepted:SendResult::busy;
}
Bytes HardwareLink::pending_tx() noexcept {
    if(active_offset_==active_.size && count_){active_=queue_[head_];head_=(head_+1)%queue_.size();--count_;active_offset_=0;}
    return Bytes(active_.data.data()+active_offset_,active_.size-active_offset_);
}
bool HardwareLink::consume_tx(std::size_t n) noexcept {
    if(n>active_.size-active_offset_)return false;
    active_offset_+=n;return true;
}
void HardwareLink::service(std::uint64_t now) noexcept {
    if(challenged_ && (now<challenge_time_||now-challenge_time_>2000000))challenged_=false;
}
void HardwareLink::reset_transport() noexcept {
    status_.active=false;status_.has_accepted=false;status_.session=0;
    rx_size_=0;discard_=false;challenged_=false;count_=0;head_=0;active_={};active_offset_=0;next_tx_=1;
    if(callbacks_.session)callbacks_.session(callbacks_.context,0,false);
}
void HardwareLink::receive(Bytes bytes,std::uint64_t now) noexcept {
    service(now);
    for(auto b:bytes){
        if(b==0){
            if(!discard_ && rx_size_){Message m;if(decode(Bytes(rx_.data(),rx_size_),decoded_,m)){++status_.frames;dispatch(m,now);}else ++status_.rejected;}
            rx_size_=0;discard_=false;
        }else if(!discard_){if(rx_size_==rx_.size()){discard_=true;rx_size_=0;++status_.oversized;}else rx_[rx_size_++]=b;}
    }
}
void HardwareLink::dispatch(const Message& m,std::uint64_t now) noexcept {
    if(m.type==std::uint8_t(Type::hello)){
        if(m.session||m.sequence||m.payload.size()!=8||read64(m.payload,0)==0){++status_.rejected;return;}
        std::array<std::uint8_t,83> reply{};
        const auto nonce=read64(m.payload,0);write64(reply,0,nonce);write64(reply,8,descriptor_.device);write64(reply,16,descriptor_.boot);
        write32(reply,24,descriptor_.firmware);write16(reply,28,descriptor_.board);write16(reply,30,descriptor_.profile);write16(reply,32,descriptor_.profile_revision);
        reply[34]=descriptor_.metadata_size;std::copy_n(descriptor_.metadata.begin(),descriptor_.metadata_size,reply.begin()+35);
        if(enqueue(std::uint8_t(Type::hello_reply),0,Bytes(reply.data(),35+descriptor_.metadata_size))){challenge_=nonce;challenge_time_=now;challenged_=true;}
        return;
    }
    if(m.type==std::uint8_t(Type::start_session)){
        if(!m.session||m.sequence||m.payload.size()!=28){++status_.rejected;return;}
        const auto nonce=read64(m.payload,0);
        const bool identity=read64(m.payload,8)==descriptor_.device && read64(m.payload,16)==descriptor_.boot && read16(m.payload,24)==descriptor_.profile && read16(m.payload,26)==descriptor_.profile_revision;
        if(!identity){++status_.rejected;return;}
        if(status_.active && m.session==status_.session){
            if(nonce==session_challenge_)enqueue(std::uint8_t(Type::ready),m.session,m.payload.first(8));else ++status_.rejected;
            return;
        }
        if(!challenged_||nonce!=challenge_){++status_.rejected;return;}
        // Already-started bytes finish their frame; queued old-session data is disposable.
        count_=0;head_=0;next_tx_=1;
        if(!enqueue(std::uint8_t(Type::ready),m.session,m.payload.first(8)))return;
        status_.active=true;status_.session=m.session;status_.has_accepted=false;status_.accepted_sequence=0;
        session_challenge_=nonce;challenged_=false;
        if(callbacks_.session)callbacks_.session(callbacks_.context,m.session,true);
        return;
    }
    if(m.type<64){++status_.rejected;return;}
    if(!status_.active||m.session!=status_.session){++status_.wrong_session;return;}
    if(status_.has_accepted && !newer(m.sequence,status_.accepted_sequence)){++status_.rejected;return;}
    if(!callbacks_.message||!callbacks_.message(callbacks_.context,m,now)){++status_.profile_rejected;return;}
    status_.accepted_sequence=m.sequence;status_.has_accepted=true;
}
}
