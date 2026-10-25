#pragma once
#include <psx_exporter/display_protocol.hpp>
#include <cmath>
#include <vector>
#include <span>
#include <stdexcept>
#include <limits>
namespace psx::recorder {
inline constexpr std::uint32_t magic=0x31465044;
inline constexpr std::size_t maximum=65520,queue_capacity=8,frame_limit=8*1024*1024,frame_header=256;
using psx::display::word;
using Batch=psx::display::Batch;
struct Header {std::uint32_t magic_value=magic;std::uint16_t version=1,kind{};std::uint32_t bytes{},sequence{};};
static_assert(sizeof(Header)==16);
inline std::uint32_t integer(const void* data,std::size_t offset) noexcept {std::uint32_t v{};std::memcpy(&v,static_cast<const std::uint8_t*>(data)+offset,4);return v;}
inline bool valid(const void* data,std::size_t length) noexcept {
    if(!data||length<32||length>maximum)return false;auto kind=integer(data,0);
    if(kind==3)return length==152;
    auto total=integer(data,4),offset=integer(data,24),part=integer(data,28);
    return (kind==1||kind==2)&&total>0&&total<=(kind==1?frame_limit:4*1024*1024)&&part==length-32&&part>0&&offset<=total&&part<=total-offset&&word(data,8)>0&&word(data,16)>0;
}
struct Decoded {std::uint64_t sequence{},paint{},generation{},start{},end{},frequency{},epoch{},capture_ticks{},allocated{};std::uint32_t kind{},assignment{},operations{},resources{},bitmap_bytes{},unit{},width{},height{},format{},instrument_id{},lower_assignment{},lower_eligibility_revision{};std::uint64_t gates_seen{},gates_yes{};};
class Reader {
    std::span<const std::uint8_t> data_;std::size_t at_{},end_{};
public:
    explicit Reader(std::span<const std::uint8_t> data):data_(data),end_(data.size()){}
    void bounds(std::size_t at,std::size_t end){if(at>end||end>data_.size())throw std::runtime_error("DPF1 bounds");at_=at;end_=end;}
    std::size_t position()const noexcept{return at_;}
    template<class T>T take(){if(sizeof(T)>end_-at_)throw std::runtime_error("DPF1 truncated operand");T v{};std::memcpy(&v,data_.data()+at_,sizeof v);at_+=sizeof v;return v;}
    template<class T>T real(){auto v=take<T>();if(!std::isfinite(v))throw std::runtime_error("DPF1 nonfinite geometry");return v;}
};
inline Decoded decode_profile(std::span<const std::uint8_t> bytes,bool instrument){
    if(bytes.size()<frame_header||bytes.size()>(instrument?1024u*1024u:frame_limit))throw std::runtime_error("Drawing frame length");
    if(instrument){
        const auto w=integer(bytes.data(),64),h=integer(bytes.data(),68);
        if(integer(bytes.data(),0)!=0x31495044u||integer(bytes.data(),4)!=1||integer(bytes.data(),8)!=bytes.size()||!w||!h||w>2048||h>2048||std::uint64_t(w)*h>262144||integer(bytes.data(),72)!=1||integer(bytes.data(),76)!=0)throw std::runtime_error("DPI1 header");
    }else if(integer(bytes.data(),0)!=magic||(integer(bytes.data(),4)!=1&&integer(bytes.data(),4)!=2)||integer(bytes.data(),8)!=bytes.size()||integer(bytes.data(),64)!=662||integer(bytes.data(),68)!=685||(integer(bytes.data(),4)==1?integer(bytes.data(),72)!=0:integer(bytes.data(),72)>=6))throw std::runtime_error("DPF1 header");
    Decoded d;d.unit=instrument?UINT32_MAX:integer(bytes.data(),72);d.instrument_id=instrument?integer(bytes.data(),72):0;d.gates_seen=word(bytes.data(),120);d.gates_yes=word(bytes.data(),136);d.width=integer(bytes.data(),64);d.height=integer(bytes.data(),68);d.format=integer(bytes.data(),4);d.kind=integer(bytes.data(),12);d.sequence=word(bytes.data(),16);d.paint=word(bytes.data(),24);d.generation=word(bytes.data(),32);d.start=word(bytes.data(),40);d.end=word(bytes.data(),48);d.frequency=word(bytes.data(),56);d.assignment=integer(bytes.data(),76);d.epoch=word(bytes.data(),80);d.capture_ticks=word(bytes.data(),104);d.allocated=word(bytes.data(),112);
    if(instrument){
        d.lower_assignment=integer(bytes.data(),192);d.lower_eligibility_revision=integer(bytes.data(),196);
        constexpr std::uint64_t eligibility=(1ull<<0)|(1ull<<3)|(1ull<<5)|(1ull<<6)|(1ull<<7);
        if(d.lower_eligibility_revision>1||
           (!d.lower_eligibility_revision&&(d.lower_assignment||d.gates_seen||d.gates_yes))||
           (d.lower_eligibility_revision==1&&(d.lower_assignment<10||d.lower_assignment>12||d.gates_seen!=eligibility||(d.gates_yes&~eligibility))))
            throw std::runtime_error("DPI1 Lower eligibility snapshot");
    }
    if(d.kind<1||d.kind>((instrument||d.format==1)?3u:6u)||!d.sequence||!d.generation||!d.frequency||d.end<d.start)throw std::runtime_error("DPF1 identity/time");
    std::array<std::uint8_t,65537> resources{};std::array<bool,17> contexts{};std::array<std::uint32_t,17> versions{};contexts[0]=true;Reader r(bytes);std::size_t at=frame_header;
    while(at<bytes.size()){r.bounds(at,bytes.size());auto code=r.take<std::uint32_t>(),length=r.take<std::uint32_t>(),context=r.take<std::uint32_t>(),version=r.take<std::uint32_t>();at=r.position();if(length>bytes.size()-at||context>=17||!contexts[context]||version!=++versions[context])throw std::runtime_error("DPF1 operation/context/version");const auto end=at+length;r.bounds(at,end);
        if(code>=1&&code<=3){auto id=r.take<std::uint32_t>();if(id!=++d.resources||id>=resources.size())throw std::runtime_error("DPF1 resource order");resources[id]=static_cast<std::uint8_t>(code);
            if(code==1){auto precision=r.take<std::uint32_t>(),winding=r.take<std::uint32_t>(),count=r.take<std::uint32_t>();if((precision!=4&&precision!=8)||winding>1||count>length/4)throw std::runtime_error("DPF1 path header");for(std::uint32_t i=0;i<count;i++){auto segment=r.take<std::uint32_t>();if(segment>4)throw std::runtime_error("DPF1 segment");const unsigned n=segment==4?0:segment==3?6:segment==2?4:2;for(unsigned j=0;j<n;j++)if(precision==4)(void)r.real<float>();else (void)r.real<double>();}}
            else if(code==2){auto width=r.real<float>();auto cap=r.take<std::uint32_t>(),join=r.take<std::uint32_t>();auto miter=r.real<float>(),phase=r.real<float>();auto count=r.take<std::uint32_t>();if(width<0||cap>2||join>2||miter<1||phase<0||count>length/4)throw std::runtime_error("DPF1 stroke");float sum=0;for(std::uint32_t i=0;i<count;i++){auto value=r.real<float>();if(value<0)throw std::runtime_error("DPF1 dash");sum+=value;}if(count&&sum==0)throw std::runtime_error("DPF1 empty dash");}
            else {
                const auto width=r.take<std::uint32_t>(),height=r.take<std::uint32_t>();
                const auto remaining=end-r.position();
                // Bound pixels by the actual frame before multiplying dimensions.
                // A forged image must never wrap its byte count into a small
                // allocation and then reach the flight-display renderer.
                if(!width||!height||width>remaining/4/height)throw std::runtime_error("DPF1 bitmap");
                const auto size=static_cast<std::uint64_t>(width)*height*4;
                if(size!=remaining)throw std::runtime_error("DPF1 bitmap");
                d.bitmap_bytes+=static_cast<std::uint32_t>(size);r.bounds(end,end);
            }
        }else if(code==4||code==5||code==6||code==11||code==14){auto id=r.take<std::uint32_t>();if(id>d.resources||(!id&&code!=6)||resources[id]!=(code==11?2:1)&&!(code==6&&id==0))throw std::runtime_error("DPF1 missing/wrong resource");}
        else if(code==7){auto child=r.take<std::uint32_t>();if(!child||child>=17||contexts[child])throw std::runtime_error("DPF1 child");contexts[child]=true;}
        else if(code==8){if(context==0)throw std::runtime_error("DPF1 root disposal");contexts[context]=false;}
        else if(code==9){for(unsigned i=0;i<6;i++)(void)r.real<double>();}
        else if(code==10)(void)r.take<std::uint32_t>();
        else if(code==12){auto rule=r.take<std::uint32_t>();auto alpha=r.real<float>();if(rule<1||rule>12||alpha<0||alpha>1)throw std::runtime_error("DPF1 composite");}
        else if(code==13){auto key=r.take<std::uint32_t>(),value=r.take<std::uint32_t>();if(key>2||value>2)throw std::runtime_error("DPF1 hint");}
        else if(code==15){auto kind=r.take<std::uint32_t>(),count=r.take<std::uint32_t>();if(kind<1||kind>13||count>100000||count!=(end-r.position())/4||(kind<=6&&count!=4)||(kind>=7&&kind<=10&&count!=6)||(kind>=11&&(count%2)))throw std::runtime_error("DPF1 primitive");for(std::uint32_t i=0;i<count;i++)(void)r.take<std::int32_t>();}
        else if(code==16){auto id=r.take<std::uint32_t>();if(!id||id>d.resources||resources[id]!=3)throw std::runtime_error("DPF1 image reference");for(unsigned i=0;i<6;i++)(void)r.real<double>();}
        else throw std::runtime_error("DPF1 unknown opcode");
        if(r.position()!=end)throw std::runtime_error("DPF1 operand consumption");at=end;d.operations++;
    }if(d.operations!=integer(bytes.data(),148)||d.resources!=integer(bytes.data(),152))throw std::runtime_error("DPF1 counts");
    if(instrument&&d.kind!=1&&(d.operations||d.resources))throw std::runtime_error("DPI1 non-drawing transition contains commands");
    return d;
}
inline Decoded decode(std::span<const std::uint8_t> bytes){return decode_profile(bytes,false);}
inline Decoded decode(const std::vector<std::uint8_t>& bytes){return decode(std::span<const std::uint8_t>(bytes));}
}
