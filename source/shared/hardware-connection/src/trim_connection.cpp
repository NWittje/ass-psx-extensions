#include "psx/hardware/trim_connection.hpp"
#include "transport.hpp"
#include <algorithm>

namespace psx::hardware {
bool decode_trim_status(Bytes bytes,std::uint8_t role,TrimDeviceStatus& out) noexcept {
    if(bytes.size()!=48||(bytes[8]&~7u)||bytes[9]>2||bytes[10]!=role||bytes[11])return false;
    TrimDeviceStatus value{};value.accepted=read32(bytes,0);value.rendered=read32(bytes,4);
    value.has_accepted=(bytes[8]&1)!=0;value.has_rendered=(bytes[8]&2)!=0;value.panel_fault=(bytes[8]&4)!=0;
    value.presentation=bytes[9];value.role=bytes[10];
    for(std::size_t i=0;i<value.diagnostics.size();++i)value.diagnostics[i]=read32(bytes,12+i*4);
    out=value;return true;
}
TrimDisplayConnection::TrimDisplayConnection():TrimDisplayConnection(detail::TransportOps{}){}
TrimDisplayConnection::TrimDisplayConnection(detail::TransportOps io)
    :connection_({this,&identify,&message},io){}
TrimDisplayConnection::~TrimDisplayConnection()=default;
OpenResult TrimDisplayConnection::open(const SerialPort& port,std::uint64_t expected_device,std::uint8_t role){
    const auto phase=connection_.status().phase;
    if(phase!=ConnectionPhase::closed&&phase!=ConnectionPhase::failed)return OpenResult::busy;
    if(role>2)return OpenResult::invalid;
    {std::lock_guard lock(gate_);device_={};role_=0;expected_role_=role;has_status_=source_connected_=false;}
    return connection_.open({port,{expected_device,0x3501,1,1},{}});
}
bool TrimDisplayConnection::identify(void* context,const Descriptor& descriptor) noexcept {
    auto& self=*static_cast<TrimDisplayConnection*>(context);std::lock_guard lock(self.gate_);
    if(descriptor.metadata_size!=33||descriptor.metadata[0]<1||descriptor.metadata[0]>2||
       (self.expected_role_&&descriptor.metadata[0]!=self.expected_role_)||
       !(std::equal(trim_artwork.begin(),trim_artwork.end(),descriptor.metadata.begin()+1)||
         std::equal(trim_artwork_legacy.begin(),trim_artwork_legacy.end(),descriptor.metadata.begin()+1)))return false;
    self.role_=descriptor.metadata[0];self.device_={};self.has_status_=false;return true;
}
bool TrimDisplayConnection::message(void* context,const Message& message,std::uint64_t) noexcept {
    auto& self=*static_cast<TrimDisplayConnection*>(context);std::lock_guard lock(self.gate_);
    if(message.type!=std::uint8_t(Type::status)||!decode_trim_status(message.payload,self.role_,self.device_))return false;
    self.has_status_=true;return true;
}
SendResult TrimDisplayConnection::publish_latest(const trim::Snapshot& snapshot) noexcept {
    {std::lock_guard lock(gate_);if(!role_||std::uint8_t(snapshot.role)!=role_)return SendResult::invalid;}
    const auto encoded=trim::encode_snapshot(snapshot);trim::Snapshot checked{};
    if(!trim::decode_snapshot(encoded,snapshot.role,checked))return SendResult::invalid;
    ProfileState message{};message.type=trim::state_type;message.size=std::uint16_t(encoded.size());
    std::copy(encoded.begin(),encoded.end(),message.payload.begin());
    const auto result=connection_.publish_latest(message);
    if(result==SendResult::accepted){std::lock_guard lock(gate_);source_connected_=(snapshot.validity&trim::source_connected)!=0;}
    return result;
}
TrimConnectionStatus TrimDisplayConnection::status() const noexcept {
    auto connection=connection_.status();std::lock_guard lock(gate_);
    return {connection,device_,role_,has_status_,source_connected_};
}
}
