#include "hwio/cpflight_protocol.hpp"
#include <algorithm>
#include <charconv>
#include <cstdio>

namespace hwio::cpflight {
namespace {
const Input definitions[]{
#include "cpflight_inputs.inc"
};
bool digits(std::string_view s){return !s.empty() && s.find_first_not_of("0123456789")==s.npos;}
std::string padded(unsigned n,unsigned width){auto s=std::to_string(n);return s.size()<=width?std::string(width-s.size(),'0')+s:std::string{};}
}
std::span<const Input> inputs() noexcept{return definitions;}
const Input* input(std::string_view code) noexcept {for(const auto& i:definitions)if(i.code==code)return &i;return nullptr;}
bool Identity::ready() const noexcept{return manufacturer && inventory && model=="CPMCP747_105" && !serial.empty() && problem.empty();}
bool Identity::receive(std::string_view s){
    if(s=="KCPF"){manufacturer=true;return true;}
    if(s.starts_with("CPMCP")){if(!model.empty() && model!=s)problem="Conflicting model identity";model=s;if(model!="CPMCP747_105")problem="Unsupported CPflight model/firmware";return true;}
    if(s.starts_with("CHIAVE")){const auto value=s.substr(6);if(value.size()<1 || value.size()>32 || !digits(value))problem="Invalid panel serial";else if(!serial.empty() && serial!=value)problem="Conflicting panel serial";else serial=value;return true;}
    if(s.starts_with("MOD")){
        inventory=true;captain=false;first_officer=false;
        auto modules=s.substr(3);if(!modules.starts_with("485")){problem="Unsupported module bus";return true;}modules.remove_prefix(3);
        while(!modules.empty()){if(modules.starts_with("EFI1")){if(captain)problem="Duplicate Captain EFIS";captain=true;modules.remove_prefix(4);}else if(modules.starts_with("EFI2")){if(first_officer)problem="Duplicate FO EFIS";first_officer=true;modules.remove_prefix(4);}else {problem="Unqualified attached CPflight module";break;}}
        return true;
    }
    return false;
}
std::optional<Numeric> numeric(std::string_view s) noexcept {
    if(s.size()<4 || s[0]!='V' || s[1]!='0' || s[2]<'2' || s[2]>'5')return {};
    Numeric n;n.channel=unsigned(s[2]-'0');auto value=s.substr(3);while(!value.empty() && value.front()==' ')value.remove_prefix(1);
    if(n.channel==2 && (value.starts_with('.') || value.starts_with("0."))){if(value.front()=='0')value.remove_prefix(1);value.remove_prefix(1);if(value.size()<2 || value.size()>3 || !digits(value))return {};int v{};std::from_chars(value.data(),value.data()+value.size(),v);n.value=value.size()==2?v*10:v;n.mach=true;if(n.value<400 || n.value>950)return {};return n;}
    bool negative=false;if(!value.empty() && (value.front()=='+' || value.front()=='-')){negative=value.front()=='-';value.remove_prefix(1);}
    if(!digits(value) || value.size()>5)return {};int v{};const auto p=std::from_chars(value.data(),value.data()+value.size(),v);if(p.ec!=std::errc{})return {};n.value=negative?-v:v;
    if((n.channel==2 && (n.value<0 || n.value>399)) || (n.channel==3 && (n.value<0 || n.value>359)) || (n.channel==4 && (n.value<0 || n.value>50000 || n.value%100)) || (n.channel==5 && (n.value<-8000 || n.value>6000 || n.value%100)))return {};
    return n;
}
std::string lamp(unsigned id,bool on){if(id==55)return on?"L1055":"L1155";if(id<21 || id>34 || id==23)return {};return std::string(on?"L01":"L11")+padded(id,2);}
std::string display(unsigned channel,std::string_view raw){
    while(!raw.empty() && raw.front()==' ')raw.remove_prefix(1);while(!raw.empty() && raw.back()==' ')raw.remove_suffix(1);if(raw.empty())return {};
    std::string out;
    if(channel==2){if(raw.starts_with("0."))raw.remove_prefix(1);if(raw.front()=='.'){auto dec=raw.substr(1);if(dec.size()!=3 || !digits(dec))return {};out="V02 ."+std::string(dec);}else {if(!digits(raw) || raw.size()>3)return {};int v{};std::from_chars(raw.data(),raw.data()+raw.size(),v);out="V02 "+padded(unsigned(v),3);}}
    else if(channel==3){if(!digits(raw) || raw.size()>3)return {};int v{};std::from_chars(raw.data(),raw.data()+raw.size(),v);if(v>359)return {};out="V03 "+padded(unsigned(v),3);}
    else if(channel==4){if(!digits(raw) || raw.size()>5)return {};int v{};std::from_chars(raw.data(),raw.data()+raw.size(),v);if(v>50000)return {};out="V04"+padded(unsigned(v),5);}
    else if(channel==5){bool neg=raw.front()=='-';if(raw.front()=='+' || neg)raw.remove_prefix(1);if(!digits(raw) || raw.size()>4)return {};int v{};std::from_chars(raw.data(),raw.data()+raw.size(),v);if(v>(neg?8000:6000) || v%100)return {};out=std::string("V05")+(neg?"-":"+")+padded(unsigned(v),4);}
    if(!out.empty())out.push_back('\0');return out;
}
}
