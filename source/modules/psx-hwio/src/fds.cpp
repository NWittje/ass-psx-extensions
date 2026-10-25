#include "hwio/fds.hpp"
#include "hwio/vendor_library.hpp"
#include "hwio/fds_replay.hpp"
#include "psx/diagnostics/journal.hpp"
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <set>

namespace hwio::fds {
constexpr std::string_view audited_api_sha256="e2570ce4875a014193848c7c621db3a6a38a88e20cc9bc2db28317197c900414";
int verified_dataline_baseline(HMODULE dll,DWORD session) {
    // SDK Set ignores a failed initial 0xff read and may otherwise write a
    // guessed complete bank. This exact-build adapter acquires all four banks
    // first, validates them, then seeds the SDK shadow without commanding outputs.
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(dll,path,32768) || sha256_file(path)!=audited_api_sha256)return -11;
    using Lookup=void*(__cdecl*)(DWORD);using Read=void(__cdecl*)(void*,int*,int*,int*,int*);
    const auto image=reinterpret_cast<std::uintptr_t>(dll);const auto lookup_address=image+0x165f0,read_address=image+0x24cc0;
    Lookup lookup{};Read read{};std::memcpy(&lookup,&lookup_address,sizeof(lookup));std::memcpy(&read,&read_address,sizeof(read));
    auto* handle=lookup(session);if(!handle)return -4;void* controller{};std::memcpy(&controller,handle,sizeof(controller));if(!controller)return -8;
    std::array<int,4> banks{-1,-1,-1,-1};read(controller,&banks[0],&banks[1],&banks[2],&banks[3]);
    if(std::any_of(banks.begin(),banks.end(),[](int v){return v<0 || v>255;}))return -7;
    for(unsigned n=0;n<banks.size();++n)std::memcpy(static_cast<std::byte*>(controller)+0xd8+n*4,&banks[n],4);
    return 0;
}
namespace {
bool natural(std::string_view s,unsigned& n,unsigned base=10) noexcept {
    const auto r=std::from_chars(s.data(),s.data()+s.size(),n,static_cast<int>(base));
    return !s.empty() && r.ec==std::errc{} && r.ptr==s.data()+s.size();
}
template<std::size_t N>bool split_fields(std::string_view payload,std::array<std::string_view,N>& parts,std::string_view delimiters=":") noexcept {
    for(std::size_t n=0;n<N;++n) {
        const auto end=payload.find_first_of(delimiters);
        if((n+1==N)!=(end==payload.npos))return false;
        parts[n]=payload.substr(0,end);payload=end==payload.npos?std::string_view{}:payload.substr(end+1);
    }
    return true;
}
bool safe_identity(std::string_view s) noexcept {
    return !s.empty() && s.size()<=64 && s.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.")==s.npos;
}
bool update1_transponder_display(const BoardCaps& caps) noexcept {
    // This firmware's future/reserved segment descriptor is one column early.
    // Physical 1234/5670 checks qualify SDK digits2..5; its alleged LEDs33..40
    // are the fourth visible digit's segments, not a keypad-light output bank.
    return std::string_view(caps.model,4)=="3305" && caps.update==1 && caps.features==0x3509 &&
        caps.resources==std::array<int,21>{8,33,40,24,1,24,4,1,4};
}
template<class T>T symbol(HMODULE dll,const char* name) {
    const auto p=GetProcAddress(dll,name);if(!p)throw std::runtime_error(std::string("Missing FDS API export: ")+name);
    T result{};static_assert(sizeof(result)==sizeof(p));std::memcpy(&result,&p,sizeof(result));return result;
}
struct Api {
    HMODULE dll{};bool opened{},fixed_serial_slots{};
    int(__cdecl* open)(){};int(__cdecl* close)(){};
    int(__cdecl* list)(char*,DWORD*,char*){};
    int(__cdecl* bind)(const char*,DWORD*){};int(__cdecl* unbind)(DWORD){};
    int(__cdecl* caps)(DWORD,BoardCaps*){};
    int(__cdecl* options)(DWORD,DWORD){};
    int(__cdecl* callback)(DWORD,bool,BOOL(CALLBACK*)(DWORD,int,int)){};
    int(__cdecl* state)(DWORD,int,int*){};
    int(__cdecl* led_enable)(DWORD,bool){};int(__cdecl* led)(DWORD,int,bool){};
    int(__cdecl* dataline_enable)(DWORD,bool){};int(__cdecl* dataline)(DWORD,int,bool){};
    int(__cdecl* segment_enable)(DWORD,bool){};int(__cdecl* segment)(DWORD,char*,int){};
    int(__cdecl* analog_enable)(DWORD,bool){};int(__cdecl* analog)(DWORD,int,int*){};
    int(__cdecl* analogs)(DWORD,unsigned char*,int){};
    int(__cdecl* brightness_enable)(DWORD,bool){};int(__cdecl* brightness)(DWORD,int){};
    ~Api(){if(opened)close();if(dll)FreeLibrary(dll);}
    void load(const std::wstring& selection) {
        const auto path=vendor_library(VendorLibrary::fds,selection).wstring();
        if(!std::filesystem::path(path).is_absolute())throw std::runtime_error("FDS USB requires an absolute API DLL path");
        DWORD ignored{};const DWORD bytes=GetFileVersionInfoSizeW(path.c_str(),&ignored);
        std::vector<std::uint8_t> info(bytes);VS_FIXEDFILEINFO* fixed{};UINT length{};
        if(!bytes || !GetFileVersionInfoW(path.c_str(),0,bytes,info.data()) ||
           !VerQueryValueW(info.data(),L"\\",reinterpret_cast<void**>(&fixed),&length) || length<sizeof(*fixed) ||
           fixed->dwFileVersionMS!=0x20000 || fixed->dwFileVersionLS!=0x10000)
            throw std::runtime_error("FDS USB requires the audited native x64 interfaceIT API 2.0.1.0");
        // This exact implementation sizes the list as count*13+1 and advances
        // every slot by13 even if a controller has an empty serial suffix.
        // Its zero padding can therefore contain an interior double NUL.
        fixed_serial_slots=sha256_file(path)==audited_api_sha256;
        dll=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll)throw std::runtime_error("Cannot load FDS x64 API: Windows error "+std::to_string(GetLastError()));
        open=symbol<decltype(open)>(dll,"interfaceIT_OpenControllers");close=symbol<decltype(close)>(dll,"interfaceIT_CloseControllers");
        list=symbol<decltype(list)>(dll,"interfaceIT_GetDeviceList");bind=symbol<decltype(bind)>(dll,"interfaceIT_Bind");
        unbind=symbol<decltype(unbind)>(dll,"interfaceIT_UnBind");caps=symbol<decltype(caps)>(dll,"interfaceIT_GetBoardInfo");
        options=symbol<decltype(options)>(dll,"interfaceIT_SetBoardOptions");
        callback=symbol<decltype(callback)>(dll,"interfaceIT_Switch_Enable_Callback");state=symbol<decltype(state)>(dll,"interfaceIT_Switch_Get_State");
        led_enable=symbol<decltype(led_enable)>(dll,"interfaceIT_LED_Enable");led=symbol<decltype(led)>(dll,"interfaceIT_LED_Set");
        dataline_enable=symbol<decltype(dataline_enable)>(dll,"interfaceIT_Dataline_Enable");dataline=symbol<decltype(dataline)>(dll,"interfaceIT_Dataline_Set");
        segment_enable=symbol<decltype(segment_enable)>(dll,"interfaceIT_7Segment_Enable");segment=symbol<decltype(segment)>(dll,"interfaceIT_7Segment_Display");
        analog_enable=symbol<decltype(analog_enable)>(dll,"interfaceIT_Analog_Enable");analog=symbol<decltype(analog)>(dll,"interfaceIT_Analog_GetValue");
        analogs=symbol<decltype(analogs)>(dll,"interfaceIT_Analog_GetValues");
        brightness_enable=reinterpret_cast<decltype(brightness_enable)>(GetProcAddress(dll,"interfaceIT_Brightness_Enable"));brightness=reinterpret_cast<decltype(brightness)>(GetProcAddress(dll,"interfaceIT_Brightness_Set"));
        // Deliberately do not resolve or call the vendor DeviceChange callback.
        const int result=open();if(result)throw std::runtime_error("FDS OpenControllers: "+std::to_string(result));opened=true;
    }
};
struct Raw {DWORD session{};std::uint64_t generation{};int index{},direction{};std::int64_t qpc{};};
SRWLOCK callback_lock=SRWLOCK_INIT;
struct CallbackQueue {
    std::array<Raw,8192> events{};
    std::array<std::pair<DWORD,std::uint64_t>,roots_bound> sessions{};
    std::size_t read{},write{};
    std::uint64_t drops{};
    HANDLE wake{};
};
CallbackQueue* active_callbacks{};
BOOL CALLBACK key_callback(DWORD session,int index,int direction) {
    AcquireSRWLockExclusive(&callback_lock);
    if(auto* queue=active_callbacks) {
        const auto match=std::find_if(queue->sessions.begin(),queue->sessions.end(),[&](const auto& p){return p.first==session;});
        if(match!=queue->sessions.end()) {
            if(queue->write-queue->read<queue->events.size())
                queue->events[queue->write++%queue->events.size()]={session,match->second,index,direction,psx::diagnostics::qpc_now()};
            else ++queue->drops;
            SetEvent(queue->wake);
        }
    }
    ReleaseSRWLockExclusive(&callback_lock);return TRUE;
}
} // namespace
bool valid_caps(const BoardCaps& c) noexcept {
    if(c.model[4] || c.manufacture_date[8] || c.update<0 || c.update>UINT16_MAX)return false;
    // SDK 2.0.1.0's native bitmap/plane writers have these address limits.
    // The final group is an unavailable servo declaration, not a writer.
    constexpr unsigned limits[]{256,128,32,32,512};
    for(unsigned group=0;group<5;++group){const auto i=group*3;const auto count=c.resources[i],first=c.resources[i+1],last=c.resources[i+2];
        if(count<0 || count>256 || (count && (first<1 || last<first || static_cast<unsigned>(last)>limits[group] || last-first+1!=count)))return false;}
    unsigned model{};
    return natural(c.model,model,16) && std::strlen(c.model)==4;
}
bool analogue_bank(const BoardCaps& c) noexcept {
    // Analog_Enable chooses the single ADC first if both feature bits exist.
    return !(c.features&0x20000) && (c.features&0x40000);
}
bool valid_usb_output(const Device& device,const Output& item) noexcept {
    const auto& intent=item.intent;const auto kind=static_cast<ResourceKind>(intent.resource);
    if(!device.input.connected || device.input.identity.backend!=BackendKind::fds_usb || item.slot!=device.input.slot || intent.device_generation!=device.input.generation)return false;
    auto declared=[&](unsigned channel){return std::any_of(device.resources.begin(),device.resources.end(),[&](const auto& resource){return resource.output && resource.kind==kind && resource.channel==channel;});};
    if(!declared(intent.channel))return false;
    if(kind==ResourceKind::boolean_output || kind==ResourceKind::dataline_output)
        return intent.channel>=1 && intent.channel<=(kind==ResourceKind::boolean_output?256:32) && std::isfinite(intent.value) && (intent.value==0 || intent.value==1);
    if(kind!=ResourceKind::integral_display)return false;
    const auto length=strnlen_s(intent.text.data(),intent.text.size());if(!length || length==intent.text.size())return false;
    unsigned digits{};bool decimal_allowed=false;
    for(std::size_t n=0;n<length;++n){const auto c=intent.text[n];if(c=='.'){if(!decimal_allowed)return false;decimal_allowed=false;}
        else {const auto channel=intent.channel+digits++;if(channel<1 || channel>32 || !declared(channel) || (c!=' ' && c!='-' && !(c>='0' && c<='9') && !(c>='A' && c<='Z')))return false;decimal_allowed=c>='0' && c<='9';}}
    return true;
}
bool generic_model(std::string_view model) noexcept {
    constexpr std::string_view models[]{"32E0","32E1","32E2","32E3","4101","4102","4103","4105","4106","4107","4109","0E03","0E06","0E0A","0E15","0E19"};
    return std::find(std::begin(models),std::end(models),model)!=std::end(models);
}
std::string_view model_name(std::string_view model) noexcept {
    // Model identities from the native MCS resource getters and Ethernet
    // radio constructors. Names do not imply a usable PSX profile.
    constexpr std::pair<std::string_view,std::string_view> names[]{
        {"32E0","SYS3"},{"32E1","SYS1"},{"32E2","SYS2"},
        {"3302","Legacy CDU; atomic bitmap keyboard"},{"3313","Legacy 747 MCP; operating map unavailable"},
        {"331A","Boeing CDU; select exact keyboard layout"},{"3318","747 RMP"},
        {"3328","DCP/EFIS; operating map unqualified"},{"3329","747 MX MCP; operating map unqualified"},
        {"4101","HIO32/64"},{"4102","HIO64/128"},{"4103","HIO128/256"},
        {"4105","HI128"},{"4106","RI8 directional encoder card"},{"4107","Relay8"},{"4109","HIO Relay8"},
        {"3306","737 electrical/pressurization panel"},{"331C","A320 ECAM"},{"3320","A320 pedestal"},
        {"3321","A320 electrical display"},{"3322","Boeing IRS panel"},{"3323","OM1"},{"3324","OE1"},{"3325","GM1"},
        {"5001","737 configurable single radio"},{"5002","737 MultiCOMM"},{"5003","737 transponder"},
        {"5004","737 configurable single radio"},{"5005","737 single NAV"},{"5006","737 single COMM"},{"5007","737 single ADF"}};
    const auto found=std::find_if(std::begin(names),std::end(names),[&](const auto& name){return name.first==model;});
    return found==std::end(names)?std::string_view{}:found->second;
}
std::string usb_key(std::string_view serial){return "fds-usb:"+std::string(serial);}
bool usb_serial_list(std::string_view bytes,bool fixed_slots,std::vector<std::string>& serials,unsigned& omitted,std::string& error){
    serials.clear();omitted=0;error.clear();std::vector<std::string> parsed;unsigned skipped{};
    auto fail=[&](const char* reason){error=reason;return false;};
    if(bytes.empty() || bytes.size()>4096 || bytes.back()!=0)return fail("Incomplete FDS serial list");
    if(bytes.size()==1)return true;
    if(fixed_slots){
        if((bytes.size()-1)%13)return fail("Invalid FDS serial slot bounds");
        for(std::size_t at=0;at+13<=bytes.size();at+=13){
            const auto slot=bytes.substr(at,13);
            if(slot[12]!=0)return fail("Unterminated FDS serial slot");
            const auto serial=slot.substr(0,12);
            if(serial.substr(0,4).find_first_not_of("0123456789ABCDEFabcdef")!=serial.npos || !safe_identity(serial)){++skipped;continue;}
            parsed.emplace_back(serial);
        }
    }else{
        if(bytes.size()<2)return fail("Incomplete FDS serial list");
        std::size_t at{};
        while(at<bytes.size() && bytes[at]){
            const auto end=bytes.find('\0',at);
            if(end==bytes.npos || end+1==bytes.size())return fail("Incomplete FDS serial list");
            const auto serial=bytes.substr(at,end-at);
            if(!safe_identity(serial))return fail("Invalid FDS serial identity");
            parsed.emplace_back(serial);at=end+1;
        }
        if(bytes.substr(at).find_first_not_of('\0')!=bytes.npos)return fail("Unexpected data after FDS serial list terminator");
    }
    serials=std::move(parsed);omitted=skipped;return true;
}
bool parse_discovery(std::string_view reply,Discovery& result,std::string& error) {
    // FDStoXP_Network 1.1.4 reads a name on line 1 and model on line 4.
    // The other fields have no qualified identity or firmware meaning.
    if(reply.empty() || reply.size()>1024){error="Invalid FDS discovery size";return false;}
    std::array<std::string_view,8> lines{};unsigned count{};
    while(!reply.empty()) {
        const auto end=reply.find('\n');auto line=reply.substr(0,end);
        if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
        if(line.empty() || line.size()>128 || count==lines.size() ||
           std::any_of(line.begin(),line.end(),[](unsigned char c){return c<32 || c>126;})) {
            error="Malformed FDS discovery fields";return false;
        }
        lines[count++]=line;reply=end==reply.npos?std::string_view{}:reply.substr(end+1);
    }
    unsigned code{};
    const auto name=count?lines[0].substr(0,lines[0].find(' ')):std::string_view{};
    if(count<4 || !safe_identity(name) || lines[3].size()!=4 || !natural(lines[3],code,16)) {
        error="FDS discovery requires a name and four-digit model";return false;
    }
    result={std::string(name),std::string(lines[3])};error.clear();return true;
}
bool parse_line(std::string_view line,Message& result,std::string& error) {
    Message m;if(line.empty() || line.size()>2048 ||
        std::any_of(line.begin(),line.end(),[](unsigned char c){return c<32 || c>126;})){error="Invalid FDS record size/characters";return false;}
    if(line.starts_with("SERIAL=")) {m.kind=Message::Kind::identity;m.serial=line.substr(7);if(!safe_identity(m.serial)){error="Invalid FDS root serial";return false;}}
    else if(line.starts_with("STATE=") || line.starts_with("USAGE=")) {
        unsigned value{};
        if(!natural(line.substr(6),value) || value>UINT16_MAX){error="Invalid FDS status value";return false;}
        m.kind=line.starts_with("STATE=")?Message::Kind::state:Message::Kind::usage;m.value=static_cast<int>(value);
    }
    else if(line.starts_with("BOARD=")) {
        std::array<std::string_view,4> parts{};
        if(!split_fields(line.substr(6),parts)){error="Malformed FDS BOARD field count";return false;}
        const auto [slot,model,revision,identity]=parts;
        unsigned code{};
        if(!natural(slot,m.board) || m.board>31 || model.size()!=4 || !natural(model,code,16) ||
           revision.empty() || identity.empty() || revision.size()>64 || identity.size()>64) {error="Malformed FDS BOARD fields";return false;}
        m.kind=Message::Kind::board;m.model=model;m.descriptor={std::string(revision),std::string(identity)};
        m.text=m.descriptor[0]+":"+m.descriptor[1];
    } else if(line.starts_with("CONFIG=")) {
        std::array<std::string_view,8> parts{};
        if(!split_fields(line.substr(7),parts,":,") || !natural(parts[0],m.board) || m.board>31 ||
           !natural(parts[3],m.range.count) || !natural(parts[5],m.range.first) || !natural(parts[7],m.range.last) ||
           m.range.count>512 || m.range.first>512 || m.range.last>512 ||
           (m.range.count?(m.range.first>m.range.last || m.range.last-m.range.first+1!=m.range.count):(m.range.first || m.range.last)) ||
           parts[1].empty() || parts[1].size()>32 || parts[2].empty() || parts[2].size()>64 || parts[4].empty() || parts[4].size()>64 || parts[6].empty() || parts[6].size()>64) {
            error="Malformed FDS resource configuration";return false;
        }
        const auto type=parts[1];
        if(type=="SWITCH")m.range.kind=ResourceKind::boolean_input;
        else if(type=="LED")m.range.kind=ResourceKind::boolean_output;
        else if(type=="7SEGMENT" || type=="7 Segment")m.range.kind=ResourceKind::integral_display;
        else if(type=="ENCODER")m.range.kind=ResourceKind::rotary_delta;
        else if(type=="ANALOG IN" || type=="ANALOG")m.range.kind=ResourceKind::analogue_input;
        else if(type=="DATALINE")m.range.kind=ResourceKind::dataline_output;
        else {m.text=line;result=std::move(m);error.clear();return true;}
        if(m.range.count && !m.range.first && m.range.kind!=ResourceKind::analogue_input){error="Invalid FDS configuration first resource";return false;}
        m.kind=Message::Kind::configuration;m.text=std::string(parts[2])+":"+std::string(parts[4])+":"+std::string(parts[6]);
    } else if(line.starts_with("BOARDFW=")) {
        std::array<std::string_view,3> parts{};
        if(!split_fields(line.substr(8),parts)){error="Malformed FDS firmware field count";return false;}
        const auto [board,firmware,revision]=parts;
        if(!natural(board,m.board) || m.board>31 || firmware.empty() || firmware.size()>64 || revision.empty() || revision.size()>64){error="Malformed FDS firmware descriptor";return false;}
        // The native client stores both strings. Neither is a USB update number.
        m.kind=Message::Kind::firmware;m.firmware=firmware;m.revision=revision;
    } else if(line.starts_with("PROMPT=")) {
        if(line.size()<8 || line.size()>71){error="Malformed FDS prompt descriptor";return false;}
        m.kind=Message::Kind::prompt;m.text=line.substr(7);
    } else if(line.front()=='B') {
        const auto equals=line.find('=');if(equals==line.npos){error="FDS incoming board records require '='";return false;}
        const auto board=line.substr(1,equals-1),payload=line.substr(equals+1);
        if(!natural(board,m.board) || m.board>31 || payload.empty()){error="Invalid FDS board record";return false;}
        const auto type_name=payload.substr(0,payload.find(':'));
        if(type_name.empty() || type_name.size()>64){error="Invalid FDS board record type";return false;}
        // Sim-Avionics SYSBoard 2.7 uses ANALOG:index,value. Keep the original
        // integer, rather than its lossy x4/16-bit Windows message conversion.
        // The distinct ANALOG:V vector contract and other extensions stay opaque.
        if(type_name=="ANALOG" && payload.starts_with("ANALOG:") && !payload.starts_with("ANALOG:V:")) {
            const auto pair=payload.substr(7);
            if(pair.find(',')!=pair.npos) {
                std::array<std::string_view,2> parts{};
                if(!split_fields(pair,parts,",") || !natural(parts[0],m.index) || m.index>512 || parts[1].empty()) {error="Invalid FDS raw analogue pair";return false;}
                const auto number=std::from_chars(parts[1].data(),parts[1].data()+parts[1].size(),m.value);
                if(number.ec!=std::errc{} || number.ptr!=parts[1].data()+parts[1].size()){error="Invalid FDS raw analogue integer";return false;}
                m.kind=Message::Kind::analogue_raw;m.text=line;result=std::move(m);error.clear();return true;
            }
        }
        // Preserve unsupported contracts; never reinterpret them as SW/EN.
        if(type_name!="SW" && type_name!="EN") {m.text=line;result=std::move(m);error.clear();return true;}
        std::array<std::string_view,3> parts{};
        if(!split_fields(payload,parts)){error="Invalid FDS input field count";return false;}
        const auto [type,index,token]=parts;
        if(!natural(index,m.index) || m.index>512){error="Invalid FDS resource address";return false;}
        if(type=="SW") {
            m.kind=Message::Kind::switch_value;
            if(!m.index){error="Invalid FDS switch index";return false;}
            if(token=="ON" || token=="DOWN")m.value=1;
            else if(token=="OFF")m.value=0;
            else {error="Unqualified FDS switch direction token";return false;}
        } else if(type=="EN") {
            m.kind=Message::Kind::rotary;
            if(!m.index){error="Invalid FDS encoder index";return false;}
            if(token=="C")m.value=1;
            else if(token=="W")m.value=-1;
            else {error="Unqualified FDS encoder direction token";return false;}
        } else m.text=std::string(line);
    } else if(line.starts_with("ERROR=")){m.kind=Message::Kind::error;m.text=line.substr(6);}
    else m.text=line;
    result=std::move(m);error.clear();return true;
}
void NetworkInventory::reset() {const auto next=revision+1;*this={};revision=next;}
bool NetworkInventory::invalidate(std::string reason) {
    if(!problem.empty())return false;problem=std::move(reason);++revision;return true;
}
bool NetworkInventory::accept(const Message& message) {
    if(!problem.empty())return false;
    const auto changed=[&]{++revision;return true;};
    switch(message.kind) {
    case Message::Kind::identity:
        if(!safe_identity(message.serial))return invalidate("Invalid FDS root serial");
        if(serial==message.serial)return false;
        if(!serial.empty())return invalidate("Root identity changed during the TCP session");
        serial=message.serial;return changed();
    case Message::Kind::state:
    case Message::Kind::usage: {
        if(message.value<0 || message.value>UINT16_MAX)return invalidate("Invalid FDS status value");
        auto& value=message.kind==Message::Kind::state?state:usage;
        if(value==message.value)return false;value=message.value;return changed();
    }
    case Message::Kind::prompt:
        if(message.text.empty() || message.text.size()>64)return invalidate("Invalid FDS prompt descriptor");
        if(prompt==message.text)return false;
        if(!prompt.empty())return invalidate("Prompt contract changed during the TCP session");
        prompt=message.text;return changed();
    case Message::Kind::error:return invalidate("FDS controller error: "+message.text.substr(0,128));
    case Message::Kind::board:
    case Message::Kind::configuration:
    case Message::Kind::firmware:break;
    default:return false; // Input edges and prompt echoes are not metadata completion.
    }
    if(message.board>=boards.size())return invalidate("FDS board inventory bound exceeded");
    auto& board=boards[message.board];
    if(message.kind==Message::Kind::board) {
        unsigned model{};
        if(message.model.size()!=4 || !natural(message.model,model,16) ||
           std::any_of(message.descriptor.begin(),message.descriptor.end(),[](const auto& s){return s.empty() || s.size()>64;}))
            return invalidate("Invalid FDS board descriptor");
        if(board.described) {
            if(board.model!=message.model || board.descriptor!=message.descriptor)return invalidate("Board descriptor changed during the TCP session");
            return false;
        }
        board.described=true;board.model=message.model;board.descriptor=message.descriptor;return changed();
    }
    if(message.kind==Message::Kind::firmware) {
        if(message.firmware.empty() || message.firmware.size()>64 || message.revision.empty() || message.revision.size()>64)
            return invalidate("Invalid FDS firmware descriptor");
        if(board.firmware_seen) {
            if(board.firmware!=message.firmware || board.revision!=message.revision)return invalidate("Board firmware changed during the TCP session");
            return false;
        }
        board.firmware_seen=true;board.firmware=message.firmware;board.revision=message.revision;return changed();
    }
    const auto& range=message.range;
    const auto kind=static_cast<unsigned>(range.kind),first_kind=static_cast<unsigned>(ResourceKind::boolean_input);
    if(kind<first_kind || kind>=first_kind+board.ranges.size())return invalidate("Unsupported FDS resource descriptor");
    const auto at=kind-first_kind;
    if(range.count>controls_bound || range.first>controls_bound || range.last>controls_bound ||
       (range.count?(range.first>range.last || range.last-range.first+1!=range.count ||
        (!range.first && range.kind!=ResourceKind::analogue_input)):(range.first || range.last)))
        return invalidate("Invalid FDS resource range");
    if(board.declared[at]) {
        const auto& previous=board.ranges[at];
        if(previous.count!=range.count || previous.first!=range.first || previous.last!=range.last)
            return invalidate("Resource range changed during the TCP session");
        return false;
    }
    unsigned inputs=range.kind==ResourceKind::boolean_input || range.kind==ResourceKind::rotary_delta || range.kind==ResourceKind::analogue_input?range.count:0;
    for(std::size_t n=0;n<board.ranges.size();++n)if(board.declared[n] &&
       (board.ranges[n].kind==ResourceKind::boolean_input || board.ranges[n].kind==ResourceKind::rotary_delta || board.ranges[n].kind==ResourceKind::analogue_input))inputs+=board.ranges[n].count;
    if(inputs>controls_bound)return invalidate("FDS board input inventory bound exceeded");
    board.ranges[at]=range;board.declared[at]=true;return changed();
}
unsigned NetworkInventory::board_count() const noexcept {
    return static_cast<unsigned>(std::count_if(boards.begin(),boards.end(),[](const auto& b){return b.described;}));
}
unsigned NetworkInventory::resource_count() const noexcept {
    unsigned count{};for(const auto& board:boards)if(board.described)for(std::size_t n=0;n<board.ranges.size();++n)if(board.declared[n])count+=board.ranges[n].count;
    return count;
}
bool NetworkInventory::output_ready(unsigned board) const noexcept {
    // FDStoXP_Network's direct writer accepts STATE=4. This qualifies output
    // service only; it says nothing about simultaneous input contact states.
    return board<boards.size() && state==4 && !serial.empty() && problem.empty() && boards[board].described;
}
bool network_output_command(const Device& device,const Output& item,std::string& command) {
    command.clear();const auto& intent=item.intent;
    if(!device.input.connected || item.slot!=device.input.slot || intent.device_generation!=device.input.generation || device.child>=32)return false;
    const auto kind=static_cast<ResourceKind>(intent.resource);
    auto owns=[&](unsigned channel){return std::any_of(device.resources.begin(),device.resources.end(),[&](const auto& r){return r.output && r.kind==kind && r.channel==channel;});};
    if(!owns(intent.channel))return false;
    if(kind==ResourceKind::boolean_output)command="LED:"+std::to_string(intent.channel)+(intent.value!=0?":ON":":OFF");
    else if(kind==ResourceKind::integral_display) {
        const auto length=strnlen_s(intent.text.data(),intent.text.size());if(!length || length==intent.text.size() || length>36)return false;
        unsigned digit{};bool decimal_allowed=false;
        for(unsigned n=0;n<length;++n){const auto c=intent.text[n];if(c=='.'){if(!decimal_allowed)return false;decimal_allowed=false;}
            else {if(!owns(intent.channel+digit++) || (c!=' ' && c!='-' && !(c>='0' && c<='9') && !(c>='A' && c<='Z')))return false;decimal_allowed=c>='0' && c<='9';}}
        command="7SEGMENT:"+std::to_string(intent.channel)+":"+std::string(intent.text.data(),length);
    } else return false;
    command="B"+std::to_string(device.child)+":"+command+"\r\n";return true;
}
std::string NetworkInventory::status() const {
    if(!problem.empty())return problem+"; reconnect required";
    if(serial.empty())return "Awaiting root SERIAL; discovery is location only";
    if(!board_count())return "Root "+serial+"; awaiting identified board descriptors";
    return "Root "+serial+"; "+std::to_string(board_count())+" board descriptors, "+std::to_string(resource_count())+
        " declared resources; "+(state==4?"output service ready; input snapshot contracts pending":"output service awaiting STATE=4; input snapshot contracts pending");
}
bool invalidate_duplicate_roots(std::span<NetworkInventory* const> roots) {
    bool changed=false;
    for(std::size_t a=0;a<roots.size();++a)if(roots[a] && !roots[a]->serial.empty())
        for(std::size_t b=a+1;b<roots.size();++b)if(roots[b] && roots[a]!=roots[b] && roots[a]->serial==roots[b]->serial) {
            changed=roots[a]->invalidate("Duplicate FDS Ethernet root serial") || changed;
            changed=roots[b]->invalidate("Duplicate FDS Ethernet root serial") || changed;
        }
    return changed;
}
struct Provider::Impl {
    Provider& owner;
    std::mutex mutex;
    std::wstring dll_path,adapter;
    std::wstring loaded_path;
    bool enabled{true},enabled_seen{true},identification_overflow{};std::vector<Identification> identifications;
    std::vector<std::string> disabled;
    std::uint64_t settings_revision{},settings_seen{UINT64_MAX},generation{},sequence{};
    std::thread worker;
    HANDLE wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)},stop_event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    HCMNOTIFICATION notification{};
    CallbackQueue callbacks;
    std::unique_ptr<Api> api;
    Cdu3302Factory cdu_factory;
    DatalineBaseline dataline_baseline;
    struct Usb {
        Device device;DWORD session{};bool replay{},led_enabled{},segments_enabled{},analog_enabled{},datalines_enabled{},datalines_known{};
        std::unique_ptr<Cdu3302Port> cdu;Cdu3302Bitmap bitmap;std::string cdu_error;
        std::array<bool,controls_bound> known{};
        std::set<unsigned> test_lamps;bool test_brightness{},test_lcd{},brightness_enabled{};
        std::array<int,controls_bound> values{};
        std::array<bool,33> known_digits{};
        std::map<std::pair<std::uint16_t,std::uint16_t>,Output> owned;
    };
    std::array<Usb,roots_bound> usb;
    struct Net {Device device;std::size_t root_index{};bool resend_pending{};std::array<bool,controls_bound> known{};std::map<std::pair<unsigned,unsigned>,Output> owned,pending;};
    std::array<Net,roots_bound> net_devices;
    std::array<std::array<AtomicSample,controls_bound>,max_fds_devices> latest;
    struct Activity {std::atomic<std::uint64_t> reports{};std::atomic<std::int64_t> last_qpc{};};
    std::array<Activity,max_fds_devices> activities{};
    std::vector<Device> published;
    Health status;
    Spsc<Output,8192> output;
    IdentificationPort output_identification;
    bool wizard_draining{};
    IdentificationLease output_lease;
    std::optional<IdentificationLease::Write> network_identification_write;
    std::size_t network_identification_root{};
    psx::network::AdapterMonitor adapters;
    psx::network::Udp udp;
    std::vector<psx::network::Endpoint> candidates;
    std::array<std::unique_ptr<psx::network::Stream>,roots_bound> streams;
    std::array<Framer,roots_bound> framing;
    std::array<NetworkInventory,roots_bound> network;std::array<std::uint64_t,roots_bound> network_generation{};
    std::vector<NetworkInventory> published_network;
    std::wstring network_adapter;
    bool network_available{};
    bool rediscover{true},usb_reset_required{};std::atomic<bool> pnp_changed{false};
    explicit Impl(Provider& p,Cdu3302Factory factory,DatalineBaseline reader):owner(p),cdu_factory(factory),dataline_baseline(reader){callbacks.wake=wake;}
    ~Impl(){CloseHandle(wake);CloseHandle(stop_event);}
    static DWORD CALLBACK changed(HCMNOTIFICATION,PVOID context,CM_NOTIFY_ACTION,PCM_NOTIFY_EVENT_DATA data,DWORD) {
        // Only FDS interfaces invalidate FDS sessions. Unrelated joystick changes
        // must not discard a current MCP baseline or blank its indications.
        if(!data || data->FilterType!=CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE)return ERROR_SUCCESS;
        const auto* path=data->u.DeviceInterface.SymbolicLink;
        bool fds_path=false;
        for(const auto* at=path;*at;++at)if(_wcsnicmp(at,L"vid_1fd1",8)==0 || _wcsnicmp(at,L"vid_07c0",8)==0){fds_path=true;break;}
        if(!fds_path)return ERROR_SUCCESS;
        auto& self=*static_cast<Impl*>(context);self.pnp_changed=true;SetEvent(self.wake);return ERROR_SUCCESS;
    }
    void publish() {
        std::vector<Device> next;for(auto& u:usb)if(!u.device.key.empty()){u.device.input.status=u.device.status;next.push_back(u.device);}for(auto& n:net_devices)if(!n.device.key.empty())next.push_back(n.device);
        std::lock_guard lock(mutex);
        const auto equal=[](const auto& a,const auto& b){return a.key==b.key && a.status==b.status && a.baseline==b.baseline && a.readouts_known==b.readouts_known && a.outputs_pending==b.outputs_pending && a.model==b.model && a.input.generation==b.input.generation && a.input.connected==b.input.connected && a.input.controls.size()==b.input.controls.size() && a.resources.size()==b.resources.size();};
        if(next.size()==published.size() && std::equal(next.begin(),next.end(),published.begin(),published.end(),equal))return;
        for(const auto& d:next)owner.trace.record(TraceKind::metadata,d.input.slot,d.input.generation,d.key+" "+d.model+" "+d.status);
        published=std::move(next);++owner.metadata_revision;++status.revision;SetEvent(owner.available_event);
    }
    void push(Usb& u,unsigned control,int value,bool baseline,bool invalid,std::int64_t stamp) {
        if(control>=u.device.input.controls.size() || control>=controls_bound)return;
        const Sample sample{u.device.input.slot,static_cast<std::uint16_t>(control),u.device.input.generation,++sequence,stamp,value,baseline,false,false,invalid};
        latest[u.device.input.slot-first_fds_device][control].store(sample);
        if(u.device.input.controls[control].key.kind==ControlKind::button && !owner.edges.push(sample))++owner.overflow;
        ++owner.reports;auto& a=activities[u.device.input.slot-first_fds_device];++a.reports;a.last_qpc=stamp;
        SetEvent(owner.available_event);
    }
    void retire(Usb& u) {
        if(!u.session)return;
        if(output_lease.active() && output_lease.card().device==u.device.key){if(!wizard_draining && !usb_reset_required)restore_wizard();output_lease.lost("USB session retired; output restoration unconfirmed");}
        AcquireSRWLockExclusive(&callback_lock);
        for(auto& entry:callbacks.sessions)if(entry.first==u.session)entry={};
        ReleaseSRWLockExclusive(&callback_lock);
        blank(u);
        if(u.cdu){u.cdu->stop();u.cdu.reset();}else api->callback(u.session,false,key_callback);
        u.bitmap.reset();
        api->unbind(u.session);u.session=0;u.device.input.connected=false;u.device.baseline=false;u.device.status="USB disconnected";
        u.known_digits.fill(false);u.device.readouts_known=false;
        u.known.fill(false);u.values.fill(0);u.led_enabled=false;u.segments_enabled=false;u.analog_enabled=false;u.datalines_enabled=false;u.datalines_known=false;u.brightness_enabled=false;u.owned.clear();
    }
    void output_fault(Usb& u,int result,std::string_view operation) {
        u.device.status="FDS "+std::string(operation)+" error "+std::to_string(result);owner.trace.fault(u.device.status);
        if(result==-7 || result==-8 || result==-4){
            // Dataline_Set mutates its cached bank before a failed USB write.
            // UnBind releases the lock/session but retains that controller
            // object. CloseControllers is required to discard the bad cache.
            usb_reset_required=true;
            for(auto& card:usb)if(card.session){card.device.input.connected=false;card.device.baseline=false;card.known.fill(false);}
            u.device.status+="; rebuilding USB SDK cache and startup baselines";
        }
        publish();
    }
    void blank(Usb& u) {
        if(!u.session || !api)return;
        bool datalines_ready=true;
        if(u.datalines_enabled && (u.test_lcd || std::any_of(u.owned.begin(),u.owned.end(),[](const auto& entry){return entry.first.first==static_cast<unsigned>(ResourceKind::dataline_output);}))) {const int result=dataline_baseline?dataline_baseline(api->dll,u.session):-11;datalines_ready=result==0;owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,"Dataline release bank readback "+std::to_string(result));if(!datalines_ready)owner.trace.fault("Dataline release unconfirmed: readback error "+std::to_string(result));}
        if(u.test_lcd && u.datalines_enabled && datalines_ready){const int result=api->dataline(u.session,cdu_lcd_power_channel,false);if(result)owner.trace.fault("CDU test LCD release unconfirmed: API error "+std::to_string(result));}u.test_lcd=false;
        for(const auto channel:u.test_lamps)if(u.led_enabled)api->led(u.session,static_cast<int>(channel),false);u.test_lamps.clear();if(u.test_brightness && api->brightness)api->brightness(u.session,0);if(u.test_brightness && u.brightness_enabled && api->brightness_enable){api->brightness_enable(u.session,false);u.brightness_enabled=false;}u.test_brightness=false;
        for(const auto& [address,owned]:u.owned) {const auto& intent=owned.intent;
            const auto kind=static_cast<ResourceKind>(address.first);
            if(kind==ResourceKind::boolean_output && u.device.model=="331A" && address.second==cdu_backlight_channel){if(u.brightness_enabled && api->brightness)api->brightness(u.session,0);}
            else if(kind==ResourceKind::boolean_output && u.led_enabled)api->led(u.session,address.second,owned.failure!=0);
            else if(kind==ResourceKind::dataline_output && u.datalines_enabled && datalines_ready){const int result=api->dataline(u.session,address.second,owned.failure!=0);if(result)owner.trace.fault("Dataline release unconfirmed: API error "+std::to_string(result));}
            else if(kind==ResourceKind::integral_display && u.segments_enabled){std::array<char,128> text{};unsigned n{};for(const auto c:intent.text){if(!c)break;if(c!='.')text[n++]=' ';}api->segment(u.session,text.data(),address.second);}
        }
        if(u.brightness_enabled && api->brightness_enable){const int result=api->brightness_enable(u.session,false);if(result)owner.trace.fault("Brightness local-control restoration failed: "+std::to_string(result));else u.brightness_enabled=false;}
    }
    void scan() {
        if(!api)return;
        bool inventory_changed=false;
        std::vector<std::string> disabled_cards;{std::lock_guard lock(mutex);disabled_cards=disabled;}
        std::array<char,4096> buffer{};DWORD size=static_cast<DWORD>(buffer.size());
        const int result=api->list(buffer.data(),&size,nullptr);owner.trace.record(TraceKind::api_result,0,generation,"enumerate result "+std::to_string(result)+" bytes "+std::to_string(size));
        auto enumeration_failed=[&](std::string reason){
            for(auto& u:usb)if(u.session){retire(u);u.device.status=reason;}
            {std::lock_guard lock(mutex);status.usb=std::move(reason);}publish();
        };
        if(result && result!=-19){enumeration_failed("FDS enumeration failed: "+std::to_string(result));return;}
        if(!result && (!size || size>buffer.size())){enumeration_failed("Invalid FDS enumeration size");return;}
        std::set<std::string> serials,duplicates;unsigned omitted{};
        std::vector<std::string> discovered;std::string serial_error;
        if(!result && !usb_serial_list(std::string_view(buffer.data(),size),api->fixed_serial_slots,discovered,omitted,serial_error)){enumeration_failed(serial_error);return;}
        if(omitted)owner.trace.record(TraceKind::api_result,0,generation,"Skipped incomplete FDS serial slots: "+std::to_string(omitted));
        for(const auto& serial:discovered) {
            if(!serials.insert(serial).second)duplicates.insert(serial);
        }
        for(const auto& serial:duplicates)serials.erase(serial);
        for(auto& u:usb)if(u.session && (!serials.count(u.device.input.identity.serial) || std::find(disabled_cards.begin(),disabled_cards.end(),u.device.key)!=disabled_cards.end())){retire(u);if(duplicates.count(u.device.input.identity.serial))u.device.status="Duplicate FDS serial; binding inhibited";else if(serials.count(u.device.input.identity.serial))u.device.status="Disabled in HWIO Hardware options";inventory_changed=true;}
        for(const auto& serial:serials) {
            if(std::find(disabled_cards.begin(),disabled_cards.end(),usb_key(serial))!=disabled_cards.end())continue;
            auto found=std::find_if(usb.begin(),usb.end(),[&](const auto& u){return u.device.input.identity.serial==serial;});
            if(found==usb.end())found=std::find_if(usb.begin(),usb.end(),[](const auto& u){return u.device.key.empty();});
            if(found==usb.end())continue;auto& u=*found;if(u.session)continue;
            inventory_changed=true;
            u.device.key=usb_key(serial);u.device.root=u.device.key;
            auto& d=u.device.input;d.slot=static_cast<std::uint16_t>(first_fds_device+(found-usb.begin()));d.coherent_reports=false;
            d.identity.backend=BackendKind::fds_usb;d.identity.hardware_key=u.device.key;d.identity.serial=serial;
            DWORD session{};const int bound=api->bind(serial.c_str(),&session);
            if(bound){u.device.status=bound==-18?"Controller occupied by another application":"FDS bind error "+std::to_string(bound);continue;}
            u.session=session;const int read=api->caps(session,&u.device.caps);
            if(read || !valid_caps(u.device.caps)){api->unbind(session);u.session=0;u.device.status="Invalid/unsupported FDS capabilities";continue;}
            const auto& caps=u.device.caps;u.device.model=caps.model;u.device.generic=generic_model(caps.model);
            // SDK1800166D0 changes session memory only. Option0 selects the
            // raw replay/callback path; option1's encoded decoder has neither
            // a virtual startup snapshot nor sound nonzero-code transitions.
            if(u.device.model=="3302" && api->options(session,0)!=0){api->unbind(session);u.session=0;u.device.status="Cannot select legacy CDU direct-key callback mode";continue;}
            // The qualified 330A/update4 relay uses P1.6/P1.7. The SDK's
            // default 128-key matrix mode takes those pins as X14/X15, so
            // successful dataline writes are overridden by scanning. FORCE64
            // is a session-memory option, not a persistent controller write.
            if(u.device.model=="330A" && caps.update==4 && api->options(session,2)!=0){api->unbind(session);u.session=0;u.device.status="Cannot select MCP 64-switch mode; relay pins unavailable";continue;}
            // The SDK requires FORCE64 for panels with fewer than128 keys.
            // These EFIS boards advertise46 contacts; select their scan mode
            // before subscribing, without changing persistent controller data.
            if((u.device.model=="330B" || u.device.model=="330C") && caps.resources[3]==46 && caps.resources[4]==1 && caps.resources[5]==46 && api->options(session,2)!=0){api->unbind(session);u.session=0;u.device.status="Cannot select EFIS 64-switch mode; input acquisition inhibited";continue;}
            u.cdu_error.clear();
            if(u.device.model=="3302" && caps.features&1 && caps.resources[4]==1 && caps.resources[5]>=56){
                u.cdu=cdu_factory?cdu_factory():nullptr;
                if(!u.cdu)u.cdu_error="Native bitmap transport unavailable";
                else if(!u.cdu->start(serial,u.cdu_error))u.cdu.reset();
            }
            d.controls.clear();u.device.resources.clear();
            d.generation=++generation;
            d.complete_module=!u.device.generic;d.identity.product=0;unsigned model{};natural(caps.model,model,16);d.identity.product=static_cast<std::uint16_t>(model);
            d.identity.revision=static_cast<std::uint16_t>(caps.update);d.identity.name="FDS "+u.device.model+" / "+serial;
            if(const auto name=model_name(u.device.model);!name.empty())d.identity.name+=" / "+std::string(name);
            d.connected=true;d.status="Awaiting actual FDS startup replay";u.device.status=d.status;
            auto resource=[&](ResourceKind kind,unsigned channel,bool input,bool output,int maximum,const std::string& label){
                if(input){ControlInfo c;c.key.backend=BackendKind::fds_usb;c.key.kind=kind==ResourceKind::analogue_input?ControlKind::axis:ControlKind::button;
                    c.key.channel_kind=static_cast<std::uint16_t>(kind);c.key.channel=static_cast<std::uint16_t>(channel);c.maximum=maximum;c.bits=maximum==4095?12:maximum==255?8:1;c.name=label;d.controls.push_back(c);}
                u.device.resources.push_back({kind,static_cast<std::uint16_t>(channel),label,"Native interfaceIT configured resource",input,output,false,0,static_cast<double>(maximum)});
            };
            const int input_last=u.cdu?70:u.device.model=="3302"?std::min(caps.resources[5],56):caps.resources[5];
            if(caps.features&1)for(int n=caps.resources[4];n<=input_last && caps.resources[3];++n)resource(ResourceKind::boolean_input,static_cast<unsigned>(n),true,false,1,"Input "+std::to_string(n));
            const bool corrected_transponder=update1_transponder_display(caps);
            if(caps.features&0x100)for(int n=caps.resources[1];n<=caps.resources[2] && caps.resources[0];++n){
                if(corrected_transponder)u.device.resources.push_back({ResourceKind::boolean_output,static_cast<std::uint16_t>(n),"Reserved display segment "+std::to_string(n),"Unavailable: advertised LED aliases the fourth LCD digit; keypad lighting is unqualified",false,false,false,0,1});
                else resource(ResourceKind::boolean_output,static_cast<unsigned>(n),false,true,1,"LED "+std::to_string(n));
            }
            if(u.device.model=="331A" && (caps.features&0x10000) && api->brightness_enable && api->brightness)
                resource(ResourceKind::boolean_output,cdu_backlight_channel,false,true,1,"CDU key backlight");
            if(caps.features&0x400)for(int n=caps.resources[7];n<=caps.resources[8] && caps.resources[6];++n)resource(ResourceKind::integral_display,static_cast<unsigned>(n+corrected_transponder),false,true,1,"Segment "+std::to_string(n+corrected_transponder));
            if(corrected_transponder)owner.trace.record(TraceKind::metadata,d.slot,d.generation,"3305/update1 physical LCD digits2..5; descriptor retained; aliased LED33..40 inhibited");
            if(caps.features&0x2000)for(int n=caps.resources[10];n<=caps.resources[11] && caps.resources[9];++n)resource(ResourceKind::dataline_output,static_cast<unsigned>(n),false,true,1,"Dataline "+std::to_string(n));
            if(caps.features&0x4000)for(int n=caps.resources[13];n<=caps.resources[14] && caps.resources[12];++n)
                u.device.resources.push_back({ResourceKind::servo_output,static_cast<std::uint16_t>(n),"Servo "+std::to_string(n),"Unavailable: SDK position packet recovered; actuator units, failure behavior and shared enable contract unqualified",false,false,false,0,1});
            if(caps.features&0x60000) {
                const bool bank=analogue_bank(caps);
                if(api->analog_enable(session,true)==0){u.analog_enabled=true;for(unsigned n=0;n<(bank?16u:1u);++n)resource(ResourceKind::analogue_input,n,true,false,bank?255:4095,"Analogue "+std::to_string(n+1));}
            }
            u.known_digits.fill(false);u.device.readouts_known=std::none_of(u.device.resources.begin(),u.device.resources.end(),[](const auto& r){return r.output && r.kind==ResourceKind::integral_display;});
            u.known.fill(false);u.replay=false;u.bitmap.reset();
            AcquireSRWLockExclusive(&callback_lock);callbacks.sessions[static_cast<std::size_t>(found-usb.begin())]={session,d.generation};ReleaseSRWLockExclusive(&callback_lock);
            if(u.cdu)u.device.status="Awaiting fresh legacy CDU bitmap banks";
            else if((caps.features&1) && api->callback(session,true,key_callback)!=0){u.device.status="Cannot subscribe to FDS inputs";retire(u);}
            else if(!(caps.features&1))u.device.baseline=true;
        }
        {std::lock_guard lock(mutex);status.usb=!duplicates.empty()?"Duplicate FDS serial; affected binding inhibited":serials.empty()?"No FDS USB controllers":"Native interfaceIT API 2.0.1.0; vendor device notifications disabled";if(omitted)status.usb+="; incomplete controller serial omitted";}
        if(inventory_changed)publish();
    }
    void callbacks_step() {
        std::array<Raw,256> batch{};std::size_t count{};std::uint64_t drops{};
        AcquireSRWLockExclusive(&callback_lock);
        while(count<batch.size() && callbacks.read<callbacks.write)batch[count++]=callbacks.events[callbacks.read++%callbacks.events.size()];
        drops=callbacks.drops;callbacks.drops=0;ReleaseSRWLockExclusive(&callback_lock);
        if(drops){owner.overflow+=drops;
            for(auto& u:usb)if(u.session){
                if(u.cdu)continue; // Native packet streams do not share this callback queue.
                if(!u.test_lamps.empty() || u.test_brightness || u.test_lcd){
                    // End the old generation's test in this worker iteration;
                    // restoration must not depend on a Runtime consumer.
                    Identification release;release.device=u.device.key;release.lamps.assign(u.test_lamps.begin(),u.test_lamps.end());release.brightness=u.test_brightness || u.test_lcd;
                    std::lock_guard lock(mutex);if(identifications.size()<16)identifications.push_back(std::move(release));else identification_overflow=true;
                }
                api->callback(u.session,false,key_callback);u.device.baseline=false;u.replay=false;u.known.fill(false);
                u.device.input.generation=++generation;
                AcquireSRWLockExclusive(&callback_lock);
                for(auto& entry:callbacks.sessions)if(entry.first==u.session)entry.second=u.device.input.generation;
                ReleaseSRWLockExclusive(&callback_lock);
                api->callback(u.session,true,key_callback);
            }publish();count=0;
        }
        bool changed_state=false;
        for(std::size_t i=0;i<count;++i) {
            const auto& event=batch[i];auto found=std::find_if(usb.begin(),usb.end(),[&](const auto& u){return u.session==event.session && u.device.input.generation==event.generation;});
            if(found==usb.end())continue;auto& u=*found;owner.trace.record(TraceKind::usb_input,u.device.input.slot,event.generation,std::to_string(event.index)+" "+std::to_string(event.direction),static_cast<std::uint64_t>(event.qpc));
            if(event.index==0) {
                if(event.direction==1){u.replay=true;u.known.fill(false);u.device.baseline=false;u.device.status="Awaiting actual FDS startup replay";changed_state=true;}
                else if(event.direction==0 && u.replay) {
                    bool valid=true;for(std::size_t c=0;c<u.device.input.controls.size();++c)if(u.device.input.controls[c].key.kind==ControlKind::button && !u.known[c])valid=false;
                    u.replay=false;u.device.baseline=valid;u.device.status=valid?(u.device.model=="3302"?"Connected; direct keys only; "+u.cdu_error:"Connected; current startup baseline"):"Incomplete startup replay";changed_state=true;
                    if(valid)for(std::size_t c=0;c<u.device.input.controls.size();++c)if(u.known[c])push(u,static_cast<unsigned>(c),u.values[c],true,false,event.qpc);
                }continue;
            }
            const auto& caps=u.device.caps;
            if(event.index<caps.resources[4] || event.index>caps.resources[5] || (u.device.model=="3302" && event.index>56)){++owner.rejected;continue;}
            const unsigned c=static_cast<unsigned>(event.index-caps.resources[4]);
            const bool valid=event.direction==0 || event.direction==1;
            u.known[c]=valid;u.values[c]=valid?event.direction:0;
            if(!u.replay && u.device.baseline)push(u,c,u.values[c],false,!valid,event.qpc);
        }
        if(changed_state)publish();
    }
    void bitmap_step() {
        bool changed_state=false;
        for(auto& u:usb)if(u.session && u.cdu){
            for(unsigned serviced=0;serviced<256;++serviced){
                std::array<std::uint8_t,8> packet{};const int read=u.cdu->read(packet);
                if(!read)break;
                if(read<0){const auto error=GetLastError();retire(u);u.device.status="Legacy CDU HID read failed (Windows "+std::to_string(error)+"); fresh bind required";owner.trace.fault(u.device.status);changed_state=true;break;}
                const auto stamp=psx::diagnostics::qpc_now();
                owner.trace.record(TraceKind::usb_bitmap,u.device.input.slot,u.device.input.generation,
                    {reinterpret_cast<const char*>(packet.data()),packet.size()},static_cast<std::uint64_t>(stamp));
                const auto decoded=u.bitmap.accept(packet);
                if(decoded.kind==Cdu3302Bitmap::Result::Kind::rejected){retire(u);u.device.status="Malformed legacy CDU bitmap; fresh bind required";changed_state=true;break;}
                const bool baseline=decoded.kind==Cdu3302Bitmap::Result::Kind::baseline;
                const auto overflow_before=owner.overflow.load();
                for(unsigned n=0;n<decoded.count;++n){const auto& edge=decoded.edges[n];const auto c=edge.channel-1;
                    u.known[c]=true;u.values[c]=edge.on;push(u,c,edge.on,baseline,false,stamp);
                }
                if(owner.overflow.load()!=overflow_before){retire(u);u.device.status="Legacy CDU edge overflow; fresh generation and bitmap required";owner.trace.fault(u.device.status);changed_state=true;break;}
                if(baseline){u.device.baseline=true;u.device.status="Connected; current 70-cell legacy CDU bitmap baseline";changed_state=true;}
            }
        }
        if(changed_state)publish();
    }
    void analog_step() {
        for(auto& u:usb)if(u.session && u.analog_enabled) {
            const bool bank=analogue_bank(u.device.caps);std::array<unsigned char,16> bytes{};int single{};
            const int result=bank?api->analogs(u.session,bytes.data(),16):api->analog(u.session,0,&single);
            for(std::size_t c=0;c<u.device.input.controls.size();++c)if(u.device.input.controls[c].key.kind==ControlKind::axis) {
                const auto& control=u.device.input.controls[c];const int value=bank?bytes[control.key.channel]:single;
                const bool invalid=result!=0 || value<0 || value>control.maximum;
                if(!u.known[c] || u.values[c]!=value || invalid){push(u,static_cast<unsigned>(c),value,!u.known[c],invalid,psx::diagnostics::qpc_now());u.known[c]=!invalid;u.values[c]=value;}
            }
        }
    }
    void outputs_step() {
        if(usb_reset_required)return;
        Output item;unsigned serviced{};
        while(serviced++<256 && output.pop(item)) {
            if(item.slot>=first_fds_device+roots_bound){network_output(item);continue;}
            if(item.slot<first_fds_device)continue;
            auto& u=usb[item.slot-first_fds_device];const auto& intent=item.intent;
            if(!u.session || !u.device.input.connected || intent.device_generation!=u.device.input.generation)continue;
            const auto kind=static_cast<ResourceKind>(intent.resource);
            if(!valid_usb_output(u.device,item)){++owner.rejected;owner.trace.record(TraceKind::routing,u.device.input.slot,u.device.input.generation,"Rejected USB output: bounds, glyphs or address");continue;}
            if(output_lease.active() && output_lease.card().device==u.device.key && output_lease.normal(intent.resource,intent.channel,intent.value!=0)){
                u.owned[{intent.resource,intent.channel}]=item;owner.trace.record(TraceKind::routing,u.device.input.slot,u.device.input.generation,"Output wizard deferred normal "+std::to_string(intent.resource)+":"+std::to_string(intent.channel));continue;
            }
            if(kind==ResourceKind::boolean_output && u.test_lamps.count(intent.channel)) {
                // Identification owns only its declared caption channels. Keep
                // the latest normal value for restoration without overwriting
                // the current one-second test phase with a queued update.
                u.owned[{intent.resource,intent.channel}]=item;
                owner.trace.record(TraceKind::routing,u.device.input.slot,u.device.input.generation,"Identification deferred normal LED "+std::to_string(intent.channel)+" "+std::to_string(intent.value!=0));
                continue;
            }
            int result=-15;
            if(kind==ResourceKind::boolean_output && u.device.model=="331A" && intent.channel==cdu_backlight_channel){
                if(u.test_brightness){u.owned[{intent.resource,intent.channel}]=item;continue;}
                if(!u.brightness_enabled){result=api->brightness_enable(u.session,true);if(!result || result==-16)u.brightness_enabled=true;}
                if(u.brightness_enabled)result=api->brightness(u.session,intent.value!=0?255:0);
                if(!result && item.release){result=api->brightness_enable(u.session,false);if(!result)u.brightness_enabled=false;}
            } else if(kind==ResourceKind::boolean_output) {
                if(u.device.model=="331A" && !u.datalines_known){result=dataline_baseline?dataline_baseline(api->dll,u.session):-11;u.datalines_known=result==0;if(result){output_fault(u,result,"CDU shared output bank baseline (no write issued)");if(usb_reset_required)break;continue;}}
                if(!u.led_enabled){result=api->led_enable(u.session,true);if(!result || result==-16)u.led_enabled=true;}
                if(u.led_enabled)result=api->led(u.session,intent.channel,intent.value!=0);
            } else if(kind==ResourceKind::dataline_output) {
                if(u.test_lcd && intent.channel==cdu_lcd_power_channel){u.owned[{intent.resource,intent.channel}]=item;continue;}
                if(!u.datalines_known){result=dataline_baseline?dataline_baseline(api->dll,u.session):-11;u.datalines_known=result==0;owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,"Dataline bank baseline "+std::to_string(result));if(!u.datalines_known){output_fault(u,result,"dataline readback (no write issued)");if(usb_reset_required)break;continue;}}
                if(u.datalines_known){if(!u.datalines_enabled){result=api->dataline_enable(u.session,true);if(!result || result==-16)u.datalines_enabled=true;}if(u.datalines_enabled)result=api->dataline(u.session,intent.channel,intent.value!=0);}
            } else if(kind==ResourceKind::integral_display) {
                if(!u.segments_enabled){result=api->segment_enable(u.session,true);if(!result || result==-16)u.segments_enabled=true;}
                if(u.segments_enabled){auto text=intent.text;result=api->segment(u.session,text.data(),intent.channel);}
            }
            owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,std::to_string(intent.resource)+" "+std::to_string(intent.channel)+" "+std::to_string(intent.value)+" "+std::to_string(result)+" "+(kind==ResourceKind::integral_display?intent.text.data():""));
            if(!result){if(item.release && u.device.model=="331A" && kind==ResourceKind::boolean_output && intent.channel==cdu_backlight_channel)u.owned.erase({intent.resource,intent.channel});else u.owned[{intent.resource,intent.channel}]=item;
                if(kind==ResourceKind::integral_display){unsigned digit=intent.channel;for(const auto c:intent.text){if(!c)break;if(c!='.')u.known_digits[digit++]=true;}
                    if(!u.device.readouts_known && std::all_of(u.device.resources.begin(),u.device.resources.end(),[&](const auto& r){return !r.output || r.kind!=ResourceKind::integral_display || u.known_digits[r.channel];})){u.device.readouts_known=true;publish();}}
            }
            else {output_fault(u,result,"output");if(usb_reset_required)break;}
            // Shared 0x14 device enable is retained until this session retires.
            // Disabling LED use while segments are active would disable both.
        }
    }
    void network_inventory_step() {
        std::set<std::string> alive;
        for(std::size_t root=0;root<roots_bound;++root)if(streams[root] && streams[root]->state()==psx::network::State::connected && network[root].problem.empty() && !network[root].serial.empty())for(unsigned board=0;board<32;++board) {
            const auto& meta=network[root].boards[board];if(!meta.described)continue;
            const auto key="fds-net:"+network[root].serial+"/"+std::to_string(board);alive.insert(key);
            std::vector<std::string> disabled_cards;{std::lock_guard lock(mutex);disabled_cards=disabled;}if(std::find(disabled_cards.begin(),disabled_cards.end(),key)!=disabled_cards.end()){alive.erase(key);continue;}
            auto found=std::find_if(net_devices.begin(),net_devices.end(),[&](const auto& d){return d.device.key==key;});if(found==net_devices.end())found=std::find_if(net_devices.begin(),net_devices.end(),[](const auto& d){return d.device.key.empty();});if(found==net_devices.end()){++owner.rejected;continue;}
            auto& n=*found;std::vector<ControlInfo> controls;std::vector<HardwareResource> resources;
            for(std::size_t kind=0;kind<meta.ranges.size();++kind)if(meta.declared[kind]){const auto& range=meta.ranges[kind];for(unsigned channel=range.first;range.count && channel<=range.last;++channel){const bool input=range.kind==ResourceKind::boolean_input || range.kind==ResourceKind::rotary_delta;const bool destination=range.kind==ResourceKind::boolean_output || range.kind==ResourceKind::integral_display;
                if(!input && !destination)continue;const auto label=range.kind==ResourceKind::rotary_delta?"Encoder ":input?"Input ":range.kind==ResourceKind::integral_display?"Digit ":"Output ";resources.push_back({range.kind,static_cast<std::uint16_t>(channel),label+std::to_string(channel),"Recovered direct Ethernet resource; no coherent cross-contact boundary",input,destination,false,range.kind==ResourceKind::rotary_delta?-1.:0.,1.});
                if(input){ControlInfo c;c.key.backend=BackendKind::fds_ethernet;c.key.kind=ControlKind::button;c.key.channel_kind=static_cast<std::uint16_t>(range.kind);c.key.channel=static_cast<std::uint16_t>(channel);c.relative=range.kind==ResourceKind::rotary_delta;c.minimum=c.relative?-1:0;c.maximum=1;c.bits=1;c.name=label+std::to_string(channel);controls.push_back(std::move(c));}}
            }
            const auto fingerprint=[](const std::vector<ControlInfo>& c){std::string key;for(const auto& x:c)key+=std::to_string(x.key.channel_kind)+":"+std::to_string(x.key.channel)+";";return key;};
            if(n.device.key.empty() || !n.device.input.connected || fingerprint(n.device.input.controls)!=fingerprint(controls) || n.device.resources.size()!=resources.size()) {
                n.device.key=key;n.device.root="fds-net:"+network[root].serial;n.device.child=board;n.root_index=root;n.device.model=meta.model;n.device.generic=generic_model(meta.model);n.device.caps={};strcpy_s(n.device.caps.model,meta.model.c_str());
                auto& d=n.device.input;d.slot=static_cast<std::uint16_t>(first_fds_device+roots_bound+(found-net_devices.begin()));d.generation=++generation;d.identity.backend=BackendKind::fds_ethernet;d.identity.hardware_key=key;d.identity.serial=network[root].serial;d.identity.name="FDS "+meta.model+" / "+network[root].serial+" / board "+std::to_string(board);d.coherent_reports=false;d.complete_module=!n.device.generic;d.connected=true;d.controls=std::move(controls);n.device.resources=std::move(resources);n.known.fill(false);n.device.baseline=false;n.owned.clear();n.pending.clear();n.device.status="Connected; tentative protocol, individual contacts require initial state";
                if(const auto name=model_name(meta.model);!name.empty())d.identity.name+=" / "+std::string(name);
                for(std::size_t c=0;c<d.controls.size();++c)if(d.controls[c].relative){Sample sample{d.slot,static_cast<std::uint16_t>(c),d.generation,++sequence,psx::diagnostics::qpc_now(),0,true};latest[d.slot-first_fds_device][c].store(sample);n.known[c]=true;}
                n.resend_pending=true;
            }
            n.device.outputs_pending=!network[root].output_ready(board);
            n.device.status=network[root].output_ready(board)?"Connected; tentative protocol, individual contact baselines":"Connected; outputs awaiting STATE=4, individual contact baselines";
        }
        for(auto& n:net_devices)if(n.device.input.connected && !alive.count(n.device.key)){n.device.input.connected=false;n.device.baseline=false;n.device.status="Ethernet disconnected or invalid inventory";n.device.input.generation=++generation;n.known.fill(false);n.owned.clear();n.pending.clear();}
        publish();
    }
    void network_input(std::size_t root,const Message& message) {
        if(message.kind!=Message::Kind::switch_value && message.kind!=Message::Kind::rotary)return;
        const auto found=std::find_if(net_devices.begin(),net_devices.end(),[&](const auto& n){return n.device.input.connected && n.root_index==root && n.device.child==message.board;});if(found==net_devices.end()){++owner.rejected;return;}auto& n=*found;
        const auto kind=message.kind==Message::Kind::switch_value?ResourceKind::boolean_input:ResourceKind::rotary_delta;
        const auto control=std::find_if(n.device.input.controls.begin(),n.device.input.controls.end(),[&](const auto& c){return c.key.channel_kind==static_cast<unsigned>(kind) && c.key.channel==message.index;});if(control==n.device.input.controls.end()){++owner.rejected;return;}
        const auto index=static_cast<unsigned>(control-n.device.input.controls.begin());const auto stamp=psx::diagnostics::qpc_now();const Sample sample{n.device.input.slot,static_cast<std::uint16_t>(index),n.device.input.generation,++sequence,stamp,message.value,!n.known[index],kind==ResourceKind::rotary_delta};n.known[index]=true;
        latest[sample.device-first_fds_device][index].store(sample);if(!owner.edges.push(sample))++owner.overflow;++owner.reports;auto& activity=activities[sample.device-first_fds_device];++activity.reports;activity.last_qpc=stamp;SetEvent(owner.available_event);
    }
    bool network_send(Net& n,std::string command) {
        if(!n.device.input.connected || !streams[n.root_index] || streams[n.root_index]->sending())return false;
        if(!streams[n.root_index]->send({reinterpret_cast<const std::uint8_t*>(command.data()),command.size()}))return false;
        owner.trace.record(TraceKind::tcp_tx,static_cast<std::uint32_t>(n.root_index),n.device.input.generation,command);return true;
    }
    void network_output(const Output& item) {
        const auto index=item.slot-first_fds_device-roots_bound;if(index>=net_devices.size())return;auto& n=net_devices[index];const auto& intent=item.intent;
        std::string command;if(!network_output_command(n.device,item,command))return;
        if(output_lease.active() && output_lease.card().device==n.device.key && output_lease.normal(intent.resource,intent.channel,intent.value!=0)){n.owned[{intent.resource,intent.channel}]=item;n.pending.erase({intent.resource,intent.channel});return;}
        if(network[n.root_index].output_ready(n.device.child) && network_send(n,command)){n.owned[{intent.resource,intent.channel}]=item;n.pending.erase({intent.resource,intent.channel});}else n.pending[{intent.resource,intent.channel}]=item;
    }
    void network_blank() {
        const auto deadline=GetTickCount64()+500;
        for(auto& n:net_devices){n.pending.clear();if(!n.device.input.connected)continue;
            if(!network[n.root_index].output_ready(n.device.child)){if(!n.owned.empty())owner.trace.fault("FDS owned Ethernet outputs could not be released: controller not STATE=4");n.owned.clear();continue;}
            for(const auto& [address,owned]:n.owned){std::string command="B"+std::to_string(n.device.child)+":";
                if(address.first==static_cast<unsigned>(ResourceKind::boolean_output))command+="LED:"+std::to_string(address.second)+(owned.failure?":ON":":OFF");
                else if(address.first==static_cast<unsigned>(ResourceKind::integral_display)){unsigned width{};for(const auto c:owned.intent.text){if(!c)break;if(c!='.')++width;}command+="7SEGMENT:"+std::to_string(address.second)+":"+std::string(width,' ');}else continue;
                command+="\r\n";auto& stream=streams[n.root_index];
                while(stream && stream->sending() && stream->state()==psx::network::State::connected && GetTickCount64()<deadline){stream->pump();if(stream->sending())WaitForSingleObject(stream->event(),1);}
                if(GetTickCount64()<deadline)network_send(n,command);else owner.trace.fault("FDS owned Ethernet output shutdown exceeded 500 ms");
            }n.owned.clear();
        }
        for(auto& stream:streams)while(stream && stream->sending() && stream->state()==psx::network::State::connected && GetTickCount64()<deadline){stream->pump();if(stream->sending())WaitForSingleObject(stream->event(),1);}
    }
    void publish_network(std::string fallback="No FDS Ethernet roots discovered") {
        std::lock_guard lock(mutex);
        std::size_t count{};bool same=true;
        for(std::size_t n=0;n<streams.size();++n)if(streams[n]) {
            if(count>=published_network.size() || network[n].revision!=published_network[count].revision || network[n].serial!=published_network[count].serial)same=false;
            ++count;
        }
        if(count!=published_network.size())same=false;
        if(same && (count || status.ethernet==fallback))return;
        std::vector<NetworkInventory> next;
        for(std::size_t n=0;n<streams.size();++n)if(streams[n])next.push_back(network[n]);
        std::string summary=std::move(fallback);
        if(next.size()==1)summary=next.front().status();
        else if(!next.empty()) {
            summary=std::to_string(next.size())+" FDS Ethernet roots; startup and profile contracts pending";
            const auto fault=std::find_if(next.begin(),next.end(),[](const auto& root){return !root.problem.empty();});
            if(fault!=next.end())summary+="; "+fault->status();
        }
        published_network=std::move(next);status.ethernet=std::move(summary);++status.revision;SetEvent(owner.available_event);
    }
    void network_step(bool force) {
        const auto inventory=adapters.snapshot();if(!inventory)return;
        std::wstring selected;{std::lock_guard lock(mutex);selected=adapter;}
        const auto found=std::find_if(inventory->adapters.begin(),inventory->adapters.end(),[&](const auto& a){return a.guid==selected && a.up;});
        const bool available=found!=inventory->adapters.end();bool changed=false;
        if(network_available!=available || network_adapter!=selected){udp.close();for(auto& stream:streams)stream.reset();candidates.clear();for(auto& frame:framing)frame.reset();for(auto& root:network)root.reset();network_adapter=selected;network_available=available;force=true;changed=true;}
        if(found==inventory->adapters.end()){publish_network("Hardware adapter unavailable");return;}
        if(force && udp.open(*found,AF_INET,30303)) {
            constexpr std::string_view probes[]{"Dang, this board is cool!\n","D\r\n"};
            for(const auto probe:probes){owner.trace.record(TraceKind::udp_tx,0,0,probe);udp.send(psx::network::Endpoint::parse("255.255.255.255",30303),{reinterpret_cast<const std::uint8_t*>(probe.data()),probe.size()});}
        }
        psx::network::Datagram packet;unsigned received{};
        while(received++<32 && udp.receive(packet)) {
            if(packet.size<1 || packet.size>1024)continue;owner.trace.record(TraceKind::udp_rx,0,0,{reinterpret_cast<const char*>(packet.bytes.data()),packet.size});
            const std::string_view reply(reinterpret_cast<const char*>(packet.bytes.data()),packet.size);
            if(reply=="Dang, this board is cool!\n" || reply=="D\r\n")continue;
            Discovery discovery;std::string error;
            if(!parse_discovery(reply,discovery,error)){++owner.rejected;continue;}
            // Use the adapter-scoped response source for location, never DNS,
            // discovery name or IP as a persistent controller identity.
            auto endpoint=psx::network::Endpoint::parse(packet.source.host(),10346);
            const auto existing=std::find_if(candidates.begin(),candidates.end(),[&](const auto& e){return e.same(endpoint);});
            if(existing!=candidates.end()){const auto n=static_cast<std::size_t>(existing-candidates.begin());if(streams[n] && streams[n]->state()!=psx::network::State::failed)continue;framing[n].reset();network[n].reset();network_generation[n]=++generation;streams[n]=std::make_unique<psx::network::Stream>();streams[n]->connect(*found,endpoint);changed=true;continue;}
            if(candidates.size()>=roots_bound)continue;candidates.push_back(endpoint);
            const auto n=candidates.size()-1;network[n].reset();network_generation[n]=++generation;streams[n]=std::make_unique<psx::network::Stream>();streams[n]->connect(*found,endpoint);changed=true;
        }
        for(std::size_t n=0;n<streams.size();++n)if(streams[n]) {
            auto& stream=*streams[n];stream.pump();const auto bytes=stream.received();
            if(!bytes.empty())owner.trace.record(TraceKind::tcp_rx,static_cast<std::uint32_t>(n),network_generation[n],{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
            framing[n].feed(bytes,[&](std::string_view line){Message message;std::string error;
                if(!parse_line(line,message,error)){++owner.rejected;if(network[n].invalidate(error)){owner.trace.fault(error);changed=true;}return;}
                const bool was_ready=network[n].state==4;
                if(message.kind==Message::Kind::analogue_raw)
                    owner.trace.record(TraceKind::routing,static_cast<std::uint32_t>(n),network_generation[n],"Uncalibrated FDS analogue board "+std::to_string(message.board)+" channel "+std::to_string(message.index)+" raw "+std::to_string(message.value)+"; diagnostics only");
                if(network[n].accept(message)){changed=true;if(was_ready && network[n].state!=4)for(auto& device:net_devices)if(device.root_index==n)for(const auto& [address,value]:device.owned)device.pending.try_emplace(address,value);network_inventory_step();}
                network_input(n,message);
            });stream.consume(bytes.size());
            if(stream.state()==psx::network::State::failed && network[n].invalidate("FDS TCP session lost")){owner.trace.fault("FDS TCP session lost");changed=true;}
        }
        if(changed) {
            std::array<NetworkInventory*,roots_bound> active{};
            for(std::size_t n=0;n<streams.size();++n)if(streams[n] && streams[n]->state()!=psx::network::State::failed)active[n]=&network[n];
            invalidate_duplicate_roots(active);
        }
        if(changed || force){network_inventory_step();publish_network();}
    }
    void identification_step() {
        if(usb_reset_required)return;
        std::vector<Identification> pending;{std::lock_guard lock(mutex);pending.swap(identifications);if(identification_overflow){for(auto& u:usb)blank(u);identification_overflow=false;owner.trace.fault("Identification queue overflow; owned outputs released");}}for(const auto& selected:pending){
        const auto found=std::find_if(usb.begin(),usb.end(),[&](const auto& u){return u.session && u.device.key==selected.device;});
        if(found==usb.end())continue;auto& u=*found;if(selected.generation && selected.generation!=u.device.input.generation)continue;
        if(selected.active && !selected.lamps.empty() && !u.device.readouts_known){
            owner.trace.record(TraceKind::routing,u.device.input.slot,u.device.input.generation,selected.allow_readout_blanking?"Unassigned identification may blank unknown shared numeric values":"Identification unavailable: shared numeric bank values unknown");
            if(!selected.allow_readout_blanking)continue;
        }
        const bool cdu_lcd=selected.brightness && u.device.model=="331A" && (u.device.caps.features&0x10000) &&
            std::any_of(u.device.resources.begin(),u.device.resources.end(),[](const auto& r){return r.output && r.kind==ResourceKind::dataline_output && r.channel==cdu_lcd_power_channel;});
        // The LCD shares its physical bank with the annunciators. Acquire the
        // bank before either test writes, and keep LCD power steady while the
        // keypad brightness alternates; a test must not cycle the monitor.
        if(cdu_lcd && (selected.active || u.test_lcd)){
            int result=0;
            if(!u.datalines_known){result=dataline_baseline?dataline_baseline(api->dll,u.session):-11;u.datalines_known=result==0;}
            if(!result && !u.datalines_enabled){result=api->dataline_enable(u.session,true);if(!result || result==-16){u.datalines_enabled=true;result=0;}}
            const auto normal=u.owned.find({static_cast<std::uint16_t>(ResourceKind::dataline_output),static_cast<std::uint16_t>(cdu_lcd_power_channel)});
            const bool powered=selected.active || (normal!=u.owned.end() && !normal->second.release && normal->second.intent.value!=0);
            if(!result)result=api->dataline(u.session,cdu_lcd_power_channel,powered);
            owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,"identify CDU LCD "+std::to_string(powered)+" result "+std::to_string(result));
            if(result){output_fault(u,result,"identification CDU LCD");if(usb_reset_required)return;continue;}
            u.test_lcd=selected.active;
            if(!selected.active && normal!=u.owned.end() && normal->second.release)u.owned.erase(normal);
        }
        for(const auto channel:selected.lamps)if(!(u.device.model=="331A" && channel==cdu_backlight_channel) && std::any_of(u.device.resources.begin(),u.device.resources.end(),[&](const auto& r){return r.output && r.kind==ResourceKind::boolean_output && r.channel==channel;})) {
            if(!selected.active && !u.test_lamps.count(channel))continue;
            const auto normal=u.owned.find({static_cast<std::uint16_t>(ResourceKind::boolean_output),static_cast<std::uint16_t>(channel)});
            const bool value=selected.active?selected.on:normal!=u.owned.end() && normal->second.intent.value!=0;
            int result{};if(!u.led_enabled){result=api->led_enable(u.session,true);if(!result || result==-16)u.led_enabled=true;}if(u.led_enabled)result=api->led(u.session,static_cast<int>(channel),value);if(selected.active)u.test_lamps.insert(channel);else u.test_lamps.erase(channel);
            owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,"identify LED "+std::to_string(channel)+" "+std::to_string(value)+" result "+std::to_string(result));if(result){output_fault(u,result,"identification LED");if(usb_reset_required)return;}
        }
        if(selected.brightness && (u.device.caps.features&0x10000) && api->brightness_enable && api->brightness) {
            const auto normal=u.owned.find({static_cast<std::uint16_t>(ResourceKind::boolean_output),static_cast<std::uint16_t>(cdu_backlight_channel)});
            const bool assigned=u.device.model=="331A" && normal!=u.owned.end() && !normal->second.release;
            const bool on=selected.active?selected.on:assigned && normal->second.intent.value!=0;
            int result=0;if(!u.brightness_enabled){result=api->brightness_enable(u.session,true);if(!result || result==-16)u.brightness_enabled=true;}if(u.brightness_enabled)result=api->brightness(u.session,on?255:0);
            u.test_brightness=selected.active;if(!selected.active && !assigned && u.brightness_enabled){const auto restored=api->brightness_enable(u.session,false);if(!result)result=restored;if(!restored){u.brightness_enabled=false;if(normal!=u.owned.end() && normal->second.release)u.owned.erase(normal);}}owner.trace.record(TraceKind::api_result,u.device.input.slot,u.device.input.generation,"identify brightness "+std::to_string(on)+" result "+std::to_string(result));if(result){output_fault(u,result,"identification brightness");if(usb_reset_required)return;}
        }}
    }
    void wizard_step(bool support) {
        auto locate=[&](std::string_view key)->Device*{for(auto& u:usb)if(u.device.key==key)return &u.device;for(auto& n:net_devices)if(n.device.key==key)return &n.device;return nullptr;};
        const auto eligible=[](const HardwareResource& r){return r.output && (r.kind==ResourceKind::boolean_output || r.kind==ResourceKind::dataline_output);};
        if(auto cmd=output_identification.take()){
            const auto& r=cmd->request;
            if(wizard_draining && r.kind!=IdentificationRequestKind::release){output_identification.complete({r,false,!output_lease.active(),"FDS output session is stopping"});}
            else
            if(r.kind!=IdentificationRequestKind::acquire){
                if(output_lease.active())output_lease.request(r);
                else output_identification.complete({r,r.kind==IdentificationRequestKind::release,true,"Output session no longer owns the card"});
            }else {
                const auto* d=locate(cmd->card.device);std::string error;std::vector<std::uint8_t> initial(cmd->card.endpoints.size());
                if(!support || !d || !d->generic || !d->input.connected || r.connection_generation!=d->input.generation || r.resource_generation!=d->input.generation)error="FDS connection or output inventory changed";
                else if(std::any_of(d->resources.begin(),d->resources.end(),[](const auto& e){return e.output && e.kind==ResourceKind::integral_display;}))error="Shared integral-display bank is not eligible for generic output identification";
                else {
                    if(static_cast<std::size_t>(std::count_if(d->resources.begin(),d->resources.end(),eligible))!=cmd->card.endpoints.size())error="Identification must reserve every eligible Boolean output on this card";
                    for(std::size_t n=0;n<cmd->card.endpoints.size();++n){const auto& e=cmd->card.endpoints[n];
                        if(e.bit!=UINT16_MAX || std::none_of(d->resources.begin(),d->resources.end(),[&](const auto& resource){return eligible(resource) && static_cast<unsigned>(resource.kind)==e.resource && resource.channel==e.channel;}))error="FDS output address is not eligible";
                        if(d->input.slot<first_fds_device+roots_bound){const auto& u=usb[d->input.slot-first_fds_device];const auto old=u.owned.find({e.resource,e.channel});if(old!=u.owned.end())initial[n]=old->second.intent.value!=0;if(!u.test_lamps.empty() || u.test_brightness)error="A module lighting test already owns this card";}
                        else {const auto& net=net_devices[d->input.slot-first_fds_device-roots_bound];const auto pending=net.pending.find({e.resource,e.channel});const auto old=net.owned.find({e.resource,e.channel});if(pending!=net.pending.end())initial[n]=pending->second.intent.value!=0;else if(old!=net.owned.end())initial[n]=old->second.intent.value!=0;if(!network[net.root_index].output_ready(d->child))error="FDS Ethernet output initialization is incomplete";}
                    }
                }
                output_lease.acquire(output_identification,std::move(*cmd),initial,std::move(error));
            }
        }
        if(!output_lease.active()){network_identification_write.reset();return;}
        auto* d=locate(output_lease.card().device);
        if(!support || !d || !d->input.connected || d->input.generation!=output_lease.card().connection_generation){output_lease.lost("FDS connection changed; output restoration unconfirmed");network_identification_write.reset();return;}
        if(network_identification_write){
            auto& stream=streams[network_identification_root];
            if(!stream || stream->state()!=psx::network::State::connected){output_lease.lost("FDS Ethernet write lost its connection; restoration unconfirmed");network_identification_write.reset();return;}
            if(stream->sending())return;
            output_lease.complete(network_identification_write->ticket,true);network_identification_write.reset();
        }
        if(d->input.slot>=first_fds_device+roots_bound){
            auto& n=net_devices[d->input.slot-first_fds_device-roots_bound];auto& stream=streams[n.root_index];
            if(!network[n.root_index].output_ready(d->child)){output_lease.lost("FDS controller left output-ready state; restoration unconfirmed");return;}
            if(!stream || stream->sending())return;
            if(const auto w=output_lease.next()){
                Output item;item.slot=d->input.slot;item.intent.device_generation=d->input.generation;item.intent.resource=w->endpoint.resource;item.intent.channel=w->endpoint.channel;item.intent.value=w->on;
                std::string command;if(!network_output_command(*d,item,command) || !network_send(n,std::move(command))){output_lease.complete(w->ticket,false,"FDS Ethernet identification send failed");return;}
                network_identification_root=n.root_index;network_identification_write=w;
            }
            return;
        }
        if(!api || usb_reset_required){output_lease.lost("USB SDK unavailable; restoration unconfirmed");return;}
        auto& u=usb[d->input.slot-first_fds_device];
        // Bound synchronous service so ordinary input acquisition remains live.
        for(unsigned count=0;count<8;++count){const auto w=output_lease.next();if(!w)break;int result{};
            const bool dataline=w->endpoint.resource==static_cast<unsigned>(ResourceKind::dataline_output);
            const bool shared_datalines=std::any_of(d->resources.begin(),d->resources.end(),[](const auto& e){return e.output && e.kind==ResourceKind::dataline_output;});
            if(shared_datalines && !u.datalines_known){result=dataline_baseline?dataline_baseline(api->dll,u.session):-11;u.datalines_known=result==0;}
            if(!result && dataline){if(!u.datalines_enabled){result=api->dataline_enable(u.session,true);if(!result || result==-16){u.datalines_enabled=true;result=0;}}if(!result)result=api->dataline(u.session,w->endpoint.channel,w->on);}
            if(!result && !dataline){if(!u.led_enabled){result=api->led_enable(u.session,true);if(!result || result==-16){u.led_enabled=true;result=0;}}if(!result)result=api->led(u.session,w->endpoint.channel,w->on);}
            owner.trace.record(TraceKind::api_result,d->input.slot,d->input.generation,"Output wizard "+std::to_string(w->endpoint.resource)+":"+std::to_string(w->endpoint.channel)+"="+std::to_string(w->on)+" result "+std::to_string(result));
            output_lease.complete(w->ticket,result==0,result?"FDS identification API error "+std::to_string(result):"");
            if(result){output_fault(u,result,"output identification");break;}
        }
    }
    void restore_wizard() {
        if(!output_lease.active())return;
        wizard_draining=true;output_lease.release();const auto deadline=GetTickCount64()+5000;
        while(output_lease.active() && !usb_reset_required && GetTickCount64()<deadline){
            for(auto& stream:streams)if(stream)stream->pump();
            wizard_step(true);if(output_lease.active())WaitForSingleObject(wake,1);
        }
        output_lease.lost("FDS session retired before output restoration completed");wizard_draining=false;
    }
    void run() {
        settings_seen=UINT64_MAX;loaded_path.clear();
        AcquireSRWLockExclusive(&callback_lock);active_callbacks=&callbacks;ReleaseSRWLockExclusive(&callback_lock);
        CM_NOTIFY_FILTER filter{};filter.cbSize=sizeof(filter);filter.FilterType=CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;HidD_GetHidGuid(&filter.u.DeviceInterface.ClassGuid);
        CM_Register_Notification(&filter,this,changed,&notification);
        std::uint64_t next_scan{},next_analog{},next_discovery{};
        while(WaitForSingleObject(stop_event,0)!=WAIT_OBJECT_0) {
            const auto now=GetTickCount64();std::uint64_t revision;std::wstring path;bool support;
            {std::lock_guard lock(mutex);revision=settings_revision;support=enabled;path=support?dll_path:L"";}
            if(!support){std::lock_guard lock(mutex);identifications.clear();}
            const bool changed_pnp=pnp_changed.exchange(false);
            if(settings_seen==UINT64_MAX || path!=loaded_path || changed_pnp || support!=enabled_seen || usb_reset_required) {
                if(!usb_reset_required)restore_wizard();
                for(auto& u:usb)retire(u);api.reset();settings_seen=revision;
                usb_reset_required=false;
                loaded_path=path;enabled_seen=support;
                if(support)try{auto loaded=std::make_unique<Api>();loaded->load(path);api=std::move(loaded);}catch(const std::exception& e){std::lock_guard lock(mutex);status.usb=e.what();}
                else {std::lock_guard lock(mutex);status.usb="FDS support disabled";}
                next_scan=0;publish();
            }
            if(revision!=settings_seen){settings_seen=revision;next_scan=0;}
            if(now>=next_scan){scan();next_scan=now+2000;}
            if(api){callbacks_step();bitmap_step();identification_step();if(now>=next_analog){analog_step();next_analog=now+20;}}
            bool force;{std::lock_guard lock(mutex);force=rediscover;rediscover=false;}
            if(now>=next_discovery){force=true;next_discovery=now+5000;}
            if(support)network_step(force);else {network_blank();udp.close();for(auto& stream:streams)stream.reset();candidates.clear();for(auto& root:network)root.reset();publish_network("FDS support disabled");network_inventory_step();}
            if(support){for(auto& n:net_devices)if(n.device.input.connected && streams[n.root_index] && !streams[n.root_index]->sending()){if(n.resend_pending)n.resend_pending=!network_send(n,"B"+std::to_string(n.device.child)+":RESEND\r\n");else if(!n.pending.empty()){const auto item=n.pending.begin()->second;network_output(item);}}outputs_step();}wizard_step(support);
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            WaitForSingleObject(wake,10);
        }
        if(notification){CM_Unregister_Notification(notification);notification=nullptr;}
        restore_wizard();
        for(auto& u:usb)retire(u);api.reset();
        network_blank();udp.close();for(auto& stream:streams)stream.reset();candidates.clear();network_inventory_step();
        for(auto& frame:framing)frame.reset();for(auto& root:network)root.reset();publish_network("FDS Ethernet stopped");
        AcquireSRWLockExclusive(&callback_lock);active_callbacks=nullptr;ReleaseSRWLockExclusive(&callback_lock);
    }
};
Provider::Provider(Cdu3302Factory factory,DatalineBaseline reader):impl_(std::make_unique<Impl>(*this,factory,reader)){}
Provider::~Provider(){stop();CloseHandle(available_event);}
std::vector<Output> Provider::offline_outputs(){if(impl_->worker.joinable())throw std::runtime_error("Offline output inspection requires a stopped provider");std::vector<Output> values;Output value;while(impl_->output.pop(value))values.push_back(value);return values;}
void Provider::start(std::wstring dll,std::wstring adapter,std::vector<std::string> disabled){configure(std::move(dll),std::move(adapter),std::move(disabled));ResetEvent(impl_->stop_event);impl_->worker=std::thread([this]{impl_->run();});}
void Provider::configure(std::wstring dll,std::wstring adapter,std::vector<std::string> disabled){std::sort(disabled.begin(),disabled.end());std::lock_guard lock(impl_->mutex);if(impl_->dll_path==dll && impl_->adapter==adapter && impl_->disabled==disabled)return;impl_->dll_path=std::move(dll);impl_->adapter=std::move(adapter);impl_->disabled=std::move(disabled);++impl_->settings_revision;SetEvent(impl_->wake);}
void Provider::stop(){SetEvent(impl_->stop_event);SetEvent(impl_->wake);if(impl_->worker.joinable())impl_->worker.join();}
void Provider::discover(){std::lock_guard lock(impl_->mutex);impl_->rediscover=true;impl_->pnp_changed=true;SetEvent(impl_->wake);}
std::vector<Device> Provider::inventory(){std::lock_guard lock(impl_->mutex);return impl_->published;}
std::vector<NetworkInventory> Provider::network_descriptors(){std::lock_guard lock(impl_->mutex);return impl_->published_network;}
std::vector<DeviceInfo> Provider::devices(){auto all=inventory();std::vector<DeviceInfo> out;for(auto& d:all)out.push_back(std::move(d.input));return out;}
std::vector<EthernetHealth> Provider::generic_health(){std::vector<EthernetHealth> out;for(const auto& d:inventory())if(d.generic){EthernetHealth h;h.key=d.key;h.name=d.input.identity.name;h.status=d.status;h.connected=d.input.connected;h.configured=d.input.connected && !d.outputs_pending;h.revision=d.input.generation;h.resources=d.resources;out.push_back(std::move(h));}return out;}
Health Provider::health(){std::lock_guard lock(impl_->mutex);return impl_->status;}
bool Provider::latest(std::size_t d,std::size_t c,Sample& sample)const noexcept {if(d<first_fds_device || d>=first_phidget_device || c>=controls_bound)return false;sample.device=static_cast<std::uint16_t>(d);sample.control=static_cast<std::uint16_t>(c);return impl_->latest[d-first_fds_device][c].read(sample);}
InputActivity Provider::activity(std::size_t d)const noexcept {InputActivity result;if(d>=first_fds_device && d<first_phidget_device){const auto& activity=impl_->activities[d-first_fds_device];result.reports=activity.reports.load();result.last_qpc=activity.last_qpc.load();}return result;}
bool Provider::submit(Output item)noexcept {const bool accepted=impl_->output.push(item);if(accepted)SetEvent(impl_->wake);return accepted;}
void Provider::release(std::span<const Output> outputs){for(auto item:outputs){item.release=true;item.intent.value=item.failure;std::array<char,128> blank{};unsigned n{};for(const auto c:item.intent.text){if(!c)break;if(c!='.')blank[n++]=' ';}item.intent.text=blank;submit(item);}}
} // namespace hwio::fds

namespace hwio::fds {
void Provider::enabled(bool support){std::lock_guard lock(impl_->mutex);if(impl_->enabled==support)return;impl_->enabled=support;++impl_->settings_revision;SetEvent(impl_->wake);}
void Provider::identify(Identification value){std::lock_guard lock(impl_->mutex);if(impl_->identifications.size()<16)impl_->identifications.push_back(std::move(value));else {impl_->identifications.clear();impl_->identification_overflow=true;}SetEvent(impl_->wake);}
bool Provider::identify_outputs(IdentificationRequest r,const IdentificationCard& c){const bool ok=impl_->output_identification.submit(r,c);if(ok)SetEvent(impl_->wake);return ok;}
std::optional<IdentificationCompletion> Provider::identification_completion(std::uint64_t session){return impl_->output_identification.poll(session);}
}
