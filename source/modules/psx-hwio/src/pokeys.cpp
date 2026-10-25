#include "hwio/pokeys.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <bit>
#include <optional>

namespace hwio::pokeys {
std::array<std::string,55> pin_functions(const Configuration& c) {
    std::array<std::string,55> result;
    for(unsigned n=0;n<result.size();++n){
        const auto mode=c.pins[n].mode&127;auto& label=result[n];
        if(!mode)label="Disabled";
        else {for(const auto& [flag,name]:std::array<std::pair<unsigned,const char*>,4>{{{2,"Digital input"},{4,"Digital output"},{8,"Analogue input"},{64,"Counter"}}})if(mode&flag){if(!label.empty())label+=" / ";label+=name;}if(mode&~78u){if(!label.empty())label+=" / ";label+="Peripheral flags "+std::to_string(mode&~78u);}}
        if(c.pins[n].mode&128)label+=" (inverted)";
    }
    auto pin=[&](unsigned number,const std::string& label){if(number && number<=result.size())result[number-1]=label;};
    for(unsigned n=0;n<c.encoders.size();++n){const auto& e=c.encoders[n];const bool fast=n<3 && ((c.fast_configuration&3)==1 || (c.fast_configuration&3)==2);if(e.enabled || fast){const auto name=(fast?"Fast encoder ":"Encoder ")+std::to_string(n+1)+(e.enabled?"":" (index reset; unavailable for turn routing)");pin(e.a,name+" A");pin(e.b,name+" B");}}
    if(c.ultra.a && c.ultra.b){pin(c.ultra.a,"Ultra-fast encoder A");pin(c.ultra.b,"Ultra-fast encoder B");pin(13,"Ultra-fast encoder index");}
    if((c.fast_configuration&3) && (c.fast_options&1))for(unsigned n=0;n<3;++n){constexpr unsigned pins[]{9,11,27};pin(pins[n],"Fast encoder "+std::to_string(n+1)+" index");}
    if(c.matrix.enabled){for(unsigned n=0;n<std::min<unsigned>(c.matrix.rows,16);++n)pin(c.matrix.row_pins[n],"Key matrix row "+std::to_string(n+1));for(unsigned n=0;n<std::min<unsigned>(c.matrix.columns,8);++n)pin(c.matrix.column_pins[n],"Key matrix column "+std::to_string(n+1));}
    for(unsigned n=0;n<6;++n)if(c.pwm.enabled&(1u<<n))pin(22-n,"PWM "+std::to_string(n+1));
    if(c.lcd.enabled){for(unsigned n:{28u,29u,30u})pin(n,"LCD control");for(unsigned n=0;n<4;++n)pin((c.lcd.pins==1?23:31)+n,"LCD data");}
    for(unsigned n=0;n<2;++n)if(c.led[n].enabled)for(unsigned j=0;j<3;++j)pin((n?23:9)+j,"LED display "+std::to_string(n+1));
    for(const auto& p:c.peripherals)if(p.enabled){if(p.kind==Channel::poext && p.address==1)for(unsigned n:{35u,36u,37u})pin(n,"PoExtBus");if(p.kind==Channel::ppm)pin(24,"PPM input");for(auto n:p.pins)pin(n,"Configured peripheral");}
    for(const auto& m:c.motion)if(m.enabled){const auto name="Motor "+std::to_string(m.axis+1);pin(m.step_pin,name+" step");pin(m.direction_pin,name+" direction");pin(m.home_pin,name+" home");pin(m.negative_limit,name+" negative limit");pin(m.positive_limit,name+" positive limit");}
    return result;
}
bool Identity::supported() const noexcept {
    return (hardware == 11 || hardware == 29 || hardware == 31) && firmware_type == 0 && !bootloader;
}
std::string Identity::key() const {
    return "pokeys:" + std::to_string(hardware) + ":" + std::to_string(serial);
}
Capabilities capabilities(const Identity& i) noexcept {
    if (!i.supported()) return {};
    Capabilities c{55,7,26,25,128,6,2,1,80,16,i.hardware == 11 ? 0u : 100u};
    c.i2c=c.spi=c.onewire=c.poil=true;
    c.ppm=i.hardware != 11;
    c.motion=i.hardware!=11 || !i.major || i.major>3 || (i.major==3 && i.minor>=1);
    c.onewire_fixed_pin=i.hardware==11;
    // Family-level protocol commands are not proof of a model's UART/CAN pins.
    return c;
}
std::uint8_t pin_capabilities(unsigned pin) noexcept {
    if(!pin || pin>55)return 0;
    // PoKeysLib PK_IsCounterAvailableByDevice, shared by the 56 and 57 families.
    constexpr std::array<unsigned,25> counters{1,2,5,6,9,11,15,16,19,20,21,22,23,24,25,26,27,28,41,42,43,44,46,48,49};
    return static_cast<std::uint8_t>(2|4|((pin>=41 && pin<=47)?8:0)|
        (std::find(counters.begin(),counters.end(),pin)!=counters.end()?64:0));
}
Packet request(std::uint8_t command, std::uint8_t option, std::uint8_t argument) noexcept {
    Packet p{}; p[0]=0xbb; p[1]=command; p[2]=option; p[3]=argument; return p;
}
void seal(Packet& p, std::uint8_t id) noexcept {
    p[6]=id; p[7]=0;
    for(std::size_t i=0;i<7;++i) p[7]=static_cast<std::uint8_t>(p[7]+p[i]);
    // The public native transport uses the seven-byte header checksum. Byte64
    // belongs to the command payload; do not overwrite it with an invented sum.
}
bool valid_reply(std::span<const std::uint8_t> r,const Packet& p) noexcept {
    if(r.size()!=64 || r[0]!=0xaa || (r[1]!=p[1] && !(p[1]==0xce && p[2]==2 && r[1]==0xcd)) || r[6]!=p[6]) return false;
    std::uint8_t sum{}; for(std::size_t i=0;i<7;++i) sum=static_cast<std::uint8_t>(sum+r[i]);
    return sum==r[7];
}
std::uint16_t be16(const Packet& p,std::size_t o) noexcept {return static_cast<std::uint16_t>((p[o]<<8)|p[o+1]);}
std::uint32_t le32(const Packet& p,std::size_t o) noexcept {
    return std::uint32_t(p[o])|(std::uint32_t(p[o+1])<<8)|(std::uint32_t(p[o+2])<<16)|(std::uint32_t(p[o+3])<<24);
}
void put32(Packet& p,std::size_t o,std::uint32_t v) noexcept {
    for(unsigned i=0;i<4;++i)p[o+i]=static_cast<std::uint8_t>(v>>(8*i));
}
bool identity(const Packet& p,Identity& out) {
    if(p[0]!=0xaa || p[1]!=0)return false;
    Identity i;
    const bool extended=p[8]=='P' && p[9]=='K' && ((p[10]=='E' && p[11]=='x') || (p[10]=='5' && p[11]=='8'));
    if(extended){
        i.serial=le32(p,12);i.hardware=p[18];i.firmware_type=p[41];i.locked=p[58]!=0;i.bootloader=p[61]!=0;
        for(std::size_t j=31;j<41 && p[j];++j)i.name+=static_cast<char>(p[j]);
        for(std::size_t j=20;j<31 && p[j];++j)i.build+=static_cast<char>(p[j]);
    }else{
        // Native Ethernet's legacy 56E identity; exclude 55 recovery and PoTLog27.
        i.serial=be16(p,2);if(i.serial<20000 || i.serial==65535 || p[4]==0x40)return false;
        i.hardware=11;i.bootloader=(p[4]&128)!=0;i.name="PoKeys56E";
    }
    i.major=static_cast<std::uint8_t>(1+((p[4]&127)>>4));i.minor=p[4]&15;i.patch=p[5];
    if(!i.serial)return false; out=std::move(i);return true;
}
Packet digital_output(std::uint64_t owned,std::uint64_t values) noexcept {
    auto p=request(0xcc,1);for(unsigned n=0;n<7;++n){p[8+n]=static_cast<std::uint8_t>(values>>(8*n));p[20+n]=static_cast<std::uint8_t>(~(owned>>(8*n)));}return p;
}
Packet pwm_output(const Packet& observed,std::uint8_t owned,const std::array<std::uint32_t,6>& values) noexcept {
    auto p=request(0xcb,1,1);std::copy(observed.begin()+8,observed.begin()+37,p.begin()+8);
    for(unsigned n=0;n<6;++n)if(owned&(1u<<n))put32(p,9+4*n,std::min(values[n],le32(observed,33)));return p;
}
Packet led_pixel(unsigned display,unsigned row,unsigned column,bool on) noexcept {
    auto p=request(0xd6,static_cast<std::uint8_t>((display?10:0)+(on?5:6)),static_cast<std::uint8_t>(row));p[4]=static_cast<std::uint8_t>(column);return p;
}
Packet poil_read(unsigned first,unsigned count) noexcept {
    auto p=request(0x82,0x10,4);const unsigned address=4*first;p[4]=static_cast<std::uint8_t>(address);p[5]=static_cast<std::uint8_t>(address>>8);p[8]=static_cast<std::uint8_t>(4*std::min(13u,count));return p;
}
Packet poil_write(unsigned slot,std::int32_t value) noexcept {
    auto p=request(0x82,0x15,4);const unsigned address=4*slot;p[4]=static_cast<std::uint8_t>(address);p[5]=static_cast<std::uint8_t>(address>>8);p[8]=4;put32(p,9,static_cast<std::uint32_t>(value));return p;
}
bool motion_reply(const Packet& reply,const Packet& request) noexcept {
    return request[1]!=0x85 || request[2]!=0 || reply[63]==static_cast<std::uint8_t>(request[3]+0x5a);
}
bool runtime_request(const Packet& p) noexcept {
    switch(p[1]){
    case 0x00:case 0x03:case 0x3a:case 0xd8:case 0x77:return true;
    case 0xcd:return p[2]<=1; // Counter writes/resets (10/11) are never acquisition.
    case 0xce:return p[2]==2 && p[3]==0;
    case 0x1c:return p[2]==0xff && p[3]==0;
    case 0xc0:return p[2]==0 && p[3]<=1;
    case 0xc4:case 0xc5:case 0x81:case 0x09:return p[2]==0;
    case 0xca:return p[2]==10 || p[2]==20;
    case 0xcb:return p[2]==0 || (p[2]==1 && p[3]==1); // duty update, preserve period/enable mask
    case 0xcc:return p[2]<=1;
    case 0xd0:case 0xd5:return p[2]==1;
    case 0xd1:return p[2]==0x20 || p[2]==0x30;
    case 0xd6:return p[2]==5 || p[2]==6 || p[2]==15 || p[2]==16;
    case 0xda:return p[2]==2 || p[2]==1; // enabled bus value R/M/W only
    case 0x76:return p[4]==0;
    case 0xdd:return p[2]==0x10 || p[2]==0x50 || p[2]==0x55;
    case 0x82:return p[2]==0 || ((p[2]==0x10 || p[2]==0x15) && p[3]==4);
    case 0x85:return p[2]==0 || p[2]==0x10 || p[2]==2 || p[2]==0x20 || p[2]==0x21 || p[2]==0x22;
    default:return false;
    }
}
void Rate::reset() noexcept {*this=Rate{};}
void Rate::cycle(double service,double now,double fixed) noexcept {
    if(!std::isfinite(service) || service<=0)return;
    samples_[next_]=service;next_=(next_+1)%samples_.size();count_=std::min(count_+1,samples_.size());
    if(service>period_ms_)++overruns_;
    if(fixed>0){period_ms_=1000.0/fixed;return;}
    if(service>period_ms_){period_ms_=std::max(period_ms_,service*1.25);next_adjust_ms_=now+2000;}
    if(count_<64 || now<next_adjust_ms_)return;
    auto sorted=samples_;std::sort(sorted.begin(),sorted.begin()+count_);
    const auto percentile=sorted[static_cast<std::size_t>(.99*static_cast<double>(count_-1))];
    const double target=std::clamp(std::ceil(percentile*1.25*10)/10,.1,1000.0);
    period_ms_=target>period_ms_?target:std::max(target,period_ms_*.9);next_adjust_ms_=now+2000;
}
void Rate::fault() noexcept {period_ms_=std::min(1000.0,std::max(4.0,period_ms_*2));count_=0;}
std::int64_t encoder_delta(std::uint32_t before,std::uint32_t after) noexcept {
    const std::uint32_t d=after-before;
    return d<=std::uint32_t(INT32_MAX)?std::int64_t(d):std::int64_t(d)-4294967296LL;
}
} // namespace hwio::pokeys
