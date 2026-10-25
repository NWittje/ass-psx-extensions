#include <psx/sync/protocol.hpp>
#include <algorithm>
namespace psx::sync {
namespace {
void put(std::span<std::uint8_t> b,std::size_t p,std::uint64_t n,unsigned width) noexcept {for(unsigned i=0;i<width;++i)b[p+i]=static_cast<std::uint8_t>(n>>(8*i));}
std::uint64_t get(std::span<const std::uint8_t>b,std::size_t p,unsigned width) noexcept {std::uint64_t n{};for(unsigned i=0;i<width;++i)n|=std::uint64_t(b[p+i])<<(8*i);return n;}
bool kind(Kind v) noexcept{return v>=Kind::hello&&v<=Kind::detach;}
bool shape(const Header& h)noexcept{return h.frequency<=1000000000000ULL&&h.observed_qpc<=0x7fffffffffffffffULL&&
 (h.kind!=Kind::frame||(h.connection&&h.family!=Family::link&&((h.flags==ordered&&h.lane==Lane::control&&h.control_floor==h.sequence)||(h.flags==replaceable&&h.lane!=Lane::control))));}
}
bool valid(const Identity& i) noexcept{return (i.instance_high||i.instance_low)&&i.pid&&i.process_birth;}
bool encode(const Header& h,std::span<const std::uint8_t> p,Frame& f) noexcept {
 f.size=0;if((h.wire_version!=1&&h.wire_version!=version)||!kind(h.kind)||static_cast<unsigned>(h.lane)>=lane_count||h.family>Family::actor||
    !valid(h.producer)||!h.frequency||!h.sequence||p.size()>max_payload||h.flags&~3u||h.flags==3||!shape(h))return false;
 auto b=std::span(f.bytes);std::fill_n(b.begin(),header_size,0);b[0]='P';b[1]='X';b[2]='N';b[3]='1';
 put(b,4,h.wire_version,2);put(b,6,static_cast<unsigned>(h.kind),2);put(b,8,static_cast<unsigned>(h.lane),2);
 put(b,10,static_cast<unsigned>(h.family),2);put(b,12,h.flags,2);put(b,14,header_size,2);put(b,16,p.size(),4);
 put(b,20,h.producer.pid,4);put(b,24,h.connection,8);put(b,32,h.sequence,8);
 put(b,40,h.producer.instance_high,8);put(b,48,h.producer.instance_low,8);put(b,56,h.producer.process_birth,8);
 put(b,64,h.frequency,8);put(b,72,h.observed_qpc,8);if(h.wire_version==1)put(b,80,h.mapping_generation,8);else{put(b,80,h.source_service,4);put(b,84,h.target_service,4);}put(b,88,h.key,8);put(b,96,h.control_floor,8);
 std::copy(p.begin(),p.end(),b.begin()+header_size);f.size=header_size+p.size();return true;
}
bool decode(std::span<const std::uint8_t>b,Header& h,std::span<const std::uint8_t>& p) noexcept {
 if(b.size()<header_size||b.size()>max_object||b[0]!='P'||b[1]!='X'||b[2]!='N'||b[3]!='1'||(get(b,4,2)!=1&&get(b,4,2)!=version)||get(b,14,2)!=header_size)return false;
 Header v;v.kind=static_cast<Kind>(get(b,6,2));v.lane=static_cast<Lane>(get(b,8,2));v.family=static_cast<Family>(get(b,10,2));v.flags=static_cast<std::uint16_t>(get(b,12,2));v.bytes=static_cast<std::uint32_t>(get(b,16,4));
 v.producer.pid=static_cast<std::uint32_t>(get(b,20,4));v.connection=get(b,24,8);v.sequence=get(b,32,8);
 v.producer.instance_high=get(b,40,8);v.producer.instance_low=get(b,48,8);v.producer.process_birth=get(b,56,8);
 v.wire_version=static_cast<std::uint16_t>(get(b,4,2));v.frequency=get(b,64,8);v.observed_qpc=get(b,72,8);if(v.wire_version==1)v.mapping_generation=get(b,80,8);else{v.source_service=static_cast<std::uint32_t>(get(b,80,4));v.target_service=static_cast<std::uint32_t>(get(b,84,4));}v.key=get(b,88,8);v.control_floor=get(b,96,8);
 if(!kind(v.kind)||static_cast<unsigned>(v.lane)>=lane_count||v.family>Family::actor||v.flags&~3u||v.flags==3||
    !valid(v.producer)||!v.frequency||!v.sequence||v.bytes!=b.size()-header_size||!shape(v))return false;
 h=v;p=b.subspan(header_size);return true;
}
bool encode_hello(const Hello& h,std::span<std::uint8_t>b,std::size_t& n) noexcept {
 n=0;if(b.size()<56||!valid(h.identity)||!h.frequency||!h.boot||h.receive_port>65528||!h.receive_port||
    (h.role!=Role::coordinator&&h.role!=Role::plugin)||h.families&~0xfeu)return false;
 std::fill_n(b.begin(),56,0);put(b,0,static_cast<unsigned>(h.role),2);put(b,2,h.receive_port,2);put(b,4,h.families,4);
 put(b,8,h.boot,8);put(b,16,h.frequency,8);put(b,24,h.identity.instance_high,8);put(b,32,h.identity.instance_low,8);
 put(b,40,h.identity.process_birth,8);put(b,48,h.identity.pid,4);n=56;return true;
}
bool decode_hello(std::span<const std::uint8_t>b,Hello& h) noexcept {
 if(b.size()!=56||get(b,52,4))return false;Hello v;v.role=static_cast<Role>(get(b,0,2));v.receive_port=static_cast<std::uint16_t>(get(b,2,2));v.families=static_cast<std::uint32_t>(get(b,4,4));
 v.boot=get(b,8,8);v.frequency=get(b,16,8);v.identity={get(b,24,8),get(b,32,8),get(b,40,8),static_cast<std::uint32_t>(get(b,48,4))};
 std::array<std::uint8_t,56> scratch{};std::size_t n{};if(!encode_hello(v,scratch,n))return false;h=v;return true;
}
bool encode_services(const ServiceAnnouncements& services,std::span<std::uint8_t> bytes)noexcept {
 if(bytes.size()!=service_announcement_bytes)return false;
 for(std::size_t i=0;i<services.size();++i){const auto& s=services[i];if(s.open&&!s.incarnation)return false;put(bytes,i*12,s.incarnation,4);put(bytes,i*12+4,s.echo,4);put(bytes,i*12+8,s.open?1:0,4);}return true;
}
bool decode_services(std::span<const std::uint8_t> bytes,ServiceAnnouncements& services)noexcept {
 if(bytes.size()!=service_announcement_bytes)return false;ServiceAnnouncements next{};
 for(std::size_t i=0;i<next.size();++i){const auto open=get(bytes,i*12+8,4);auto& s=next[i];s.incarnation=std::uint32_t(get(bytes,i*12,4));s.echo=std::uint32_t(get(bytes,i*12+4,4));if(open>1||(open&&!s.incarnation))return false;s.open=open!=0;}services=next;return true;
}
Lane classify(Family f,std::span<const std::uint8_t>b,bool& control,std::uint64_t& key) noexcept {
 control=true;key=0;if(b.size()<16)return Lane::control;
 const auto k=get(b,6,2);key=k;
 // Semantic mailbox indices are read only from a complete current typed frame.
 // A zero key rejects malformed frames before they can alias a valid mailbox.
 const auto framed=[&](std::uint32_t magic,unsigned version,std::size_t payload){return b.size()==16+payload&&get(b,0,4)==magic&&get(b,4,2)==version&&get(b,8,4)==payload;};
 const auto invalid=[&]{control=true;key=0;return Lane::control;};
 switch(f){
 case Family::observation:
  // Session/selection/Follow and audio edges are ordered prerequisites for state.
  if(k==9||k==11){control=false;return Lane::pose;}
  if(k==3){control=false;if(b.size()>=90)key|=get(b,88,2)<<16;return Lane::pose;}
  if(k==12){control=false;return Lane::audio;}
  if(k==4||k==5||k==8||k==15){control=false;return Lane::status;}break;
 case Family::terrain:
  if(k==9){
   // PXT3 ProbeDetail: five footprint stations and two nonassembly purposes.
   if(!framed(0x33545850u,3,772))return invalid();
   const auto purpose=get(b,208,4),assembly=get(b,784,4);
   if(purpose>2||(purpose==0?assembly>=5:assembly!=0xffffffffu))return invalid();
   key|=(purpose==0?assembly:purpose+4)<<16;
  }
  if(k==3||k==4||(k>=7&&k!=12)){control=false;return Lane::terrain;}break;
 case Family::aircraft:
  if(k==3){control=false;if(b.size()>=100)key=(get(b,96,4)<<16)|k;return Lane::appearance;}
  if(k==6){if(!framed(0x31415850u,1,360)||get(b,96,4)>=6)return invalid();key|=get(b,96,4)<<16;}
  if(k==6||k==7){control=false;return Lane::status;}break;
 case Family::environment:if(k==3||k==6||k==7||k==9||k==10||k==11){
  // Time and weather reports must keep separate latest slots: time is updated
  // every SDK callback, weather at1Hz. One kind-only key starves weather evidence.
  if((k==6||k==11)&&b.size()>=(k==11?764u:760u))key|=get(b,k==11?760:756,4)<<16;
  control=false;return Lane::environment;}break;
 case Family::status:if(k==3){control=false;return Lane::status;}break;
 case Family::traffic:
  if(k==8){if(!framed(0x31565850u,1,432)||get(b,88,4)>=7)return invalid();key|=get(b,88,4)<<16;}
  if(k==3||k==8){control=false;return Lane::traffic;}if(k==5||k==6){control=false;return Lane::status;}break;
 case Family::actor:break;
 default:break;
 }return Lane::control;
}
std::uint32_t period_ms(Lane l,std::uint64_t key) noexcept {
 switch(l){case Lane::pose:case Lane::terrain:case Lane::traffic:return 10;
 case Lane::audio:return 20;case Lane::appearance:return (((key>>16)&65535u)<2)?20:50;
 case Lane::environment:return 200;case Lane::status:return 250;default:return 0;}
}
}
