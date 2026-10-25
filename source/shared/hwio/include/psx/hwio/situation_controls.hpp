#pragma once
#include "actuators.hpp"
#include <cstring>
#include <optional>
namespace psx::hwio::situation_controls {
// Optional read-only payload in the existing actuator-v3 reserved bytes.
// Older producers leave these bytes zero; no motor command or ABI is changed.
// Opaque continuity tokens cover all contacts of a compound physical selector.
// Version 2 adds a gear-only validity flag for a single applied DOWN contact.
// Its inactive value is UP; UP/OFF are indistinguishable for situation recall.
// Version 1 readers reject version 2 and use their existing PSX fallback.
inline constexpr std::uint32_t valid=1,gear_down_only=2;
struct Value {std::int32_t value{};std::uint32_t valid{};std::int64_t input_qpc{};
  std::uint64_t source{},generation{},mapping{},sequence{};};
static_assert(sizeof(Value)==48);
struct Snapshot {
  std::uint32_t signature{0x31435048};std::uint16_t version{1},bytes{320};
  std::uint64_t epoch{};std::int64_t sampled_qpc{};std::uint64_t frequency{};
  std::array<Value,6> controls{}; // gear, flap lever, thrust levers 1..4
};
static_assert(sizeof(Snapshot)==320 && sizeof(Snapshot)<=sizeof(actuators::Packet::padding));
inline void put(actuators::Packet& packet,const Snapshot& snapshot){std::memcpy(packet.padding.data(),&snapshot,sizeof(snapshot));}
inline std::optional<Snapshot> get(const actuators::Packet& packet,std::int64_t now,std::uint64_t frequency){
  Snapshot result;std::memcpy(&result,packet.padding.data(),sizeof(result));
  if(result.signature!=0x31435048||(result.version!=1&&result.version!=2)||result.bytes!=sizeof(Snapshot)||!result.epoch||!frequency||result.frequency!=frequency||result.sampled_qpc<=0||result.sampled_qpc>now||now-result.sampled_qpc>std::int64_t(frequency*2))return {};
  for(unsigned i=0;i<6;++i){const auto& value=result.controls[i];
    const bool down_only=i==0&&result.version==2&&value.valid==(valid|gear_down_only);
    if((value.valid>1&&!down_only)||(down_only&&value.value!=1&&value.value!=3)||(value.valid&&(!value.source||!value.generation||!value.mapping||!value.sequence||value.input_qpc<=0||value.input_qpc>result.sampled_qpc||value.value<(i==0?1:i==1?0:-8925)||value.value>(i==0?3:i==1?6:5000))))return {};}
  return result;
}
}
