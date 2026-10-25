#include "hwio/flightillusion.hpp"
#include <Windows.h>
#include <setupapi.h>
#include <devpkey.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace hwio::flightillusion {
std::optional<std::int64_t> local_pc_epoch(std::int64_t utc_ms) noexcept {
    if(utc_ms<946684800000LL||utc_ms>=2966371200000LL)return {};
    ULARGE_INTEGER ticks{};ticks.QuadPart=116444736000000000ULL+std::uint64_t(utc_ms)*10000;
    FILETIME utc_file{ticks.LowPart,ticks.HighPart},local_file{};SYSTEMTIME utc{},local{};DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if(GetDynamicTimeZoneInformation(&zone)==TIME_ZONE_ID_INVALID||!FileTimeToSystemTime(&utc_file,&utc)||!SystemTimeToTzSpecificLocalTimeEx(&zone,&utc,&local)||!SystemTimeToFileTime(&local,&local_file))return {};
    ticks.LowPart=local_file.dwLowDateTime;ticks.HighPart=local_file.dwHighDateTime;
    return std::int64_t(ticks.QuadPart/10000)-11644473600000LL;
}
namespace {
std::wstring wide(std::string_view s){if(s.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(!n)throw std::runtime_error("Invalid UTF-8");std::wstring out(std::size_t(n),0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n);return out;}
std::string narrow(std::wstring_view s){if(s.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("Invalid device text");std::string out(std::size_t(n),0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;}
std::string error_text(DWORD e){return "Windows error "+std::to_string(e);}
struct Handle {HANDLE h{INVALID_HANDLE_VALUE};~Handle(){if(h!=INVALID_HANDLE_VALUE&&h)CloseHandle(h);}Handle()=default;explicit Handle(HANDLE v):h(v){}Handle(const Handle&)=delete;};
std::optional<std::string> read_file(const std::filesystem::path& p,std::string& e){std::error_code ec;if(!std::filesystem::exists(p,ec)){if(ec)e=ec.message();return {};}
    const auto size=std::filesystem::file_size(p,ec);if(ec||size>262144){e="Cannot read bounded Flight Illusion configuration";return {};}
    std::ifstream in(p,std::ios::binary);if(!in){e="Cannot open Flight Illusion configuration";return {};}
    std::string text((std::istreambuf_iterator<char>(in)),{});if(in.bad()||text.size()!=size){e="Configuration changed while reading";return {};}return text;}
class FileStore final:public Store {
    std::filesystem::path path_;std::optional<std::string> expected_;bool loaded_{},valid_{};
public:
    explicit FileStore(std::filesystem::path p):path_(std::filesystem::absolute(p).lexically_normal()){}
    bool load(Configuration& c,std::string& e)override {e.clear();expected_=read_file(path_,e);loaded_=true;if(!e.empty())return false;if(!expected_){c={};valid_=true;return true;}valid_=parse_configuration(*expected_,c,e);return valid_;}
    bool save(const Configuration& c,std::string& e)override {
        e.clear();if(!loaded_||!valid_){e="Repair the existing configuration before saving";return false;}if(!valid_configuration(c,e))return false;
        std::uint64_t hash=1469598103934665603ULL;for(auto ch:path_.wstring()){hash^=unsigned(towlower(ch));hash*=1099511628211ULL;}
        Handle mutex(CreateMutexW(nullptr,FALSE,(L"Local\\PSX.FlightIllusion.Config."+std::to_wstring(hash)).c_str()));if(!mutex.h){e=error_text(GetLastError());return false;}
        const auto wait=WaitForSingleObject(mutex.h,1000);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){e="Flight Illusion configuration is busy";return false;}
        struct Unlock {HANDLE h;~Unlock(){ReleaseMutex(h);}} unlock{mutex.h};
        auto actual=read_file(path_,e);if(!e.empty())return false;if(actual!=expected_){e="Configuration was changed by another writer; reload before saving";return false;}
        const auto text=configuration_text(c);static std::atomic_uint64_t serial{};
        auto temp=path_;temp+=L".pending."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial);
        Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(file.h==INVALID_HANDLE_VALUE){e=error_text(GetLastError());return false;}
        DWORD wrote{};bool ok=WriteFile(file.h,text.data(),DWORD(text.size()),&wrote,nullptr)&&wrote==text.size()&&FlushFileBuffers(file.h);DWORD code=ok?ERROR_SUCCESS:GetLastError();CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;
        if(ok){actual=read_file(path_,e);ok=e.empty()&&actual==expected_;if(!ok&&e.empty())e="Configuration changed before commit";}
        if(ok){const auto flags=MOVEFILE_WRITE_THROUGH|(expected_?MOVEFILE_REPLACE_EXISTING:0);ok=MoveFileExW(temp.c_str(),path_.c_str(),flags)!=FALSE;if(!ok)code=GetLastError();}
        if(!ok){DeleteFileW(temp.c_str());if(e.empty())e=error_text(code);return false;}expected_=text;return true;
    }
};
class Serial final:public Link {
    HANDLE file_{INVALID_HANDLE_VALUE};OVERLAPPED read_{},write_{};std::array<std::uint8_t,4096> rx_{};std::array<std::uint8_t,6> tx_{};DWORD tx_size_{};bool reading_{},writing_{};
    int finish(OVERLAPPED& o,bool& pending,DWORD& n,std::string& e){if(GetOverlappedResult(file_,&o,&n,FALSE)){pending=false;return 1;}const auto code=GetLastError();if(code==ERROR_IO_INCOMPLETE)return 0;pending=false;e=error_text(code);return -1;}
public:
    Serial(){read_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);write_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!read_.hEvent||!write_.hEvent){if(read_.hEvent)CloseHandle(read_.hEvent);if(write_.hEvent)CloseHandle(write_.hEvent);throw std::runtime_error("Cannot create serial completion events");}}
    ~Serial()override{close();CloseHandle(read_.hEvent);CloseHandle(write_.hEvent);}
    bool open(const Port& p,std::string& e)override {close();file_=CreateFileW(wide(p.path).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);if(file_==INVALID_HANDLE_VALUE){e=error_text(GetLastError());return false;}
        DCB d{};d.DCBlength=sizeof(d);if(!GetCommState(file_,&d)){e=error_text(GetLastError());close();return false;}
        d.BaudRate=38400;d.ByteSize=8;d.Parity=NOPARITY;d.StopBits=TWOSTOPBITS;d.fBinary=TRUE;d.fParity=FALSE;d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;d.fDsrSensitivity=FALSE;d.fOutX=FALSE;d.fInX=FALSE;d.fNull=FALSE;d.fErrorChar=FALSE;d.fAbortOnError=FALSE;
        COMMTIMEOUTS t{MAXDWORD,MAXDWORD,10,0,1000};if(!SetCommState(file_,&d)||!SetCommTimeouts(file_,&t)){e=error_text(GetLastError());close();return false;}return true;}
    int read(std::span<std::uint8_t> to,std::string& e)override {if(file_==INVALID_HANDLE_VALUE){e="Port is closed";return -1;}DWORD n{};if(reading_){const auto result=finish(read_,reading_,n,e);if(result<=0)return result;}
        else {ResetEvent(read_.hEvent);if(!ReadFile(file_,rx_.data(),DWORD(std::min(to.size(),rx_.size())),&n,&read_)){if(GetLastError()==ERROR_IO_PENDING){reading_=true;return 0;}e=error_text(GetLastError());return -1;}}
        if(n>to.size()){e="Serial read exceeds destination";return -1;}std::copy_n(rx_.begin(),n,to.begin());return int(n);}
    int write(std::span<const std::uint8_t> from,std::string& e)override {if(file_==INVALID_HANDLE_VALUE){e="Port is closed";return -1;}DWORD n{};if(writing_){const auto result=finish(write_,writing_,n,e);if(result<=0)return result;return int(n);}
        tx_size_=DWORD(std::min(from.size(),tx_.size()));std::copy_n(from.begin(),tx_size_,tx_.begin());ResetEvent(write_.hEvent);if(!WriteFile(file_,tx_.data(),tx_size_,&n,&write_)){if(GetLastError()==ERROR_IO_PENDING){writing_=true;return 0;}e=error_text(GetLastError());return -1;}return int(n);}
    void close()noexcept override {if(file_!=INVALID_HANDLE_VALUE){CancelIoEx(file_,nullptr);DWORD n{};if(reading_)GetOverlappedResult(file_,&read_,&n,TRUE);if(writing_)GetOverlappedResult(file_,&write_,&n,TRUE);CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}reading_=writing_=false;}
};
}
std::shared_ptr<Store> file_store(std::filesystem::path p){return std::make_shared<FileStore>(std::move(p));}
std::vector<Port> enumerate_ports(){
    const GUID com={0x86e0d1e0,0x8089,0x11d0,{0x9c,0xe4,0x08,0x00,0x3e,0x30,0x1f,0x73}};
    auto set=SetupDiGetClassDevsW(&com,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);if(set==INVALID_HANDLE_VALUE)return {};
    struct Release {HDEVINFO h;~Release(){SetupDiDestroyDeviceInfoList(h);}} release{set};std::vector<Port> out;
    for(DWORD i=0;;++i){SP_DEVICE_INTERFACE_DATA face{};face.cbSize=sizeof(face);if(!SetupDiEnumDeviceInterfaces(set,nullptr,&com,i,&face))break;DWORD needed{};SetupDiGetDeviceInterfaceDetailW(set,&face,nullptr,0,&needed,nullptr);if(!needed||needed>65536)continue;
        std::vector<std::uint8_t> bytes(needed);auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data());detail->cbSize=sizeof(*detail);SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);if(!SetupDiGetDeviceInterfaceDetailW(set,&face,detail,needed,nullptr,&device))continue;
        auto key=SetupDiOpenDevRegKey(set,&device,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);if(key==INVALID_HANDLE_VALUE)continue;std::array<wchar_t,64> name{};DWORD type{},size=DWORD(sizeof(name));const auto result=RegQueryValueExW(key,L"PortName",nullptr,&type,reinterpret_cast<BYTE*>(name.data()),&size);RegCloseKey(key);if(result!=ERROR_SUCCESS||type!=REG_SZ||size<2||size>sizeof(name))continue;name.back()=0;
        std::wstring port=name.data();if(!port.starts_with(L"COM")||port.size()<4||!std::all_of(port.begin()+3,port.end(),[](auto x){return x>=L'0'&&x<=L'9';}))continue;
        std::array<wchar_t,512> id{},friendly{};if(!SetupDiGetDeviceInstanceIdW(set,&device,id.data(),DWORD(id.size()),nullptr))continue;
        SetupDiGetDeviceRegistryPropertyW(set,&device,SPDRP_FRIENDLYNAME,nullptr,reinterpret_cast<BYTE*>(friendly.data()),DWORD(sizeof(friendly)),nullptr);friendly.back()=0;
        out.push_back({narrow(port),narrow(friendly.data()),narrow(id.data()),narrow(detail->DevicePath)});}
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::stoul(a.name.substr(3))<std::stoul(b.name.substr(3));});return out;
}
Driver native_driver(){return {enumerate_ports,[]{return std::make_unique<Serial>();}};}
} // namespace hwio::flightillusion
