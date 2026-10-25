#include "hwio/fds_cdu.hpp"
#include <hidsdi.h>
#include <setupapi.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace hwio::fds {
Cdu3302Bitmap::Result Cdu3302Bitmap::accept(std::span<const std::uint8_t> bytes) noexcept {
    Result out;
    if(bytes.size()!=8){reset();out.kind=Result::Kind::rejected;return out;}
    if(bytes[0]!=0x19 && bytes[0]!=0x1a)return out;
    const bool upper=bytes[0]==0x1a;
    const unsigned first=upper?32u:0u, direct=upper?24u:32u;
    auto next=values_;
    for(unsigned n=0;n<direct;++n)next[first+n]=(bytes[1+n/8]&(1u<<(n%8)))!=0;
    if(upper)for(unsigned group=0;group<2;++group){
        const unsigned code=(bytes[4]>>(3*group))&7u;
        for(unsigned n=1;n<=7;++n)next[56+group*7+n-1]=code==n;
    }
    const bool was_ready=ready();banks_|=upper?2u:1u;
    if(!was_ready){
        values_=next;out.kind=ready()?Result::Kind::baseline:Result::Kind::waiting;
        if(ready())for(unsigned n=0;n<70;++n)out.edges[out.count++]={n+1,values_[n]};
        return out;
    }
    // Release before assert, including nonzero-to-nonzero encoded transitions.
    for(unsigned n=0;n<70;++n)if(values_[n] && !next[n])out.edges[out.count++]={n+1,false};
    for(unsigned n=0;n<70;++n)if(!values_[n] && next[n])out.edges[out.count++]={n+1,true};
    values_=next;out.kind=Result::Kind::changed;return out;
}
bool cdu3302_collection(unsigned vendor,unsigned product,unsigned input_bytes,
                        unsigned output_bytes,std::wstring_view hid_serial,
                        std::string_view sdk_serial) noexcept {
    // SDK1800289A0 identifies these USB transports. SDK180028C20 places the
    // five-byte collection in endpoint0; endpoint1 carries eight-byte packets.
    // SDK18001C070 prefixes the eight-character HID serial with four PID hex digits.
    if((vendor!=0x1fd1 && vendor!=0x07c0) ||
       (product!=0x03e8 && product!=0x03ea && product!=0x1583) ||
       input_bytes!=8 || output_bytes!=8 || hid_serial.size()!=8 || sdk_serial.size()!=12)return false;
    constexpr char hex[]="0123456789ABCDEF";
    for(unsigned n=0;n<4;++n)if(sdk_serial[n]!=hex[(product>>(12-4*n))&15])return false;
    for(unsigned n=0;n<8;++n)if(hid_serial[n]<32 || hid_serial[n]>126 || sdk_serial[n+4]!=hid_serial[n])return false;
    return true;
}
namespace {
class NativePort final:public Cdu3302Port {
    HANDLE file_{INVALID_HANDLE_VALUE};
    OVERLAPPED reading_{},writing_{};
    std::array<std::uint8_t,8> buffer_{};
    bool pending_{},enabled_{};
    bool command(unsigned opcode,unsigned value=0) noexcept {
        std::array<std::uint8_t,8> packet{};packet[0]=static_cast<std::uint8_t>(opcode);packet[1]=static_cast<std::uint8_t>(value);
        ResetEvent(writing_.hEvent);DWORD bytes{};
        if(!WriteFile(file_,packet.data(),static_cast<DWORD>(packet.size()),&bytes,&writing_)){
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            if(WaitForSingleObject(writing_.hEvent,50)!=WAIT_OBJECT_0){
                CancelIoEx(file_,&writing_);GetOverlappedResult(file_,&writing_,&bytes,TRUE);return false;
            }
            if(!GetOverlappedResult(file_,&writing_,&bytes,FALSE))return false;
        }
        return bytes==packet.size();
    }
public:
    ~NativePort() override {stop();}
    bool start(std::string_view serial,std::string& error) override {
        stop();GUID guid{};HidD_GetHidGuid(&guid);
        const auto devices=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
        if(devices==INVALID_HANDLE_VALUE){error="Cannot enumerate legacy CDU HID collections";return false;}
        std::wstring selected;unsigned matches{};
        SP_DEVICE_INTERFACE_DATA face{};face.cbSize=sizeof(face);
        for(DWORD n=0;SetupDiEnumDeviceInterfaces(devices,nullptr,&guid,n,&face);++n){
            DWORD bytes{};SetupDiGetDeviceInterfaceDetailW(devices,&face,nullptr,0,&bytes,nullptr);
            if(bytes<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes>65536)continue;
            std::vector<std::uint8_t> storage(bytes);
            auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailW(devices,&face,detail,bytes,nullptr,nullptr))continue;
            const auto handle=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(handle==INVALID_HANDLE_VALUE)continue;
            HIDD_ATTRIBUTES attributes{};attributes.Size=sizeof(attributes);HIDP_CAPS caps{};PHIDP_PREPARSED_DATA data{};
            std::array<wchar_t,64> identity{};
            const bool identified=HidD_GetAttributes(handle,&attributes) && HidD_GetSerialNumberString(handle,identity.data(),static_cast<ULONG>(sizeof(identity))) &&
                HidD_GetPreparsedData(handle,&data);
            const bool match=identified && HidP_GetCaps(data,&caps)==HIDP_STATUS_SUCCESS &&
                cdu3302_collection(attributes.VendorID,attributes.ProductID,caps.InputReportByteLength,caps.OutputReportByteLength,
                    {identity.data(),static_cast<std::size_t>(std::find(identity.begin(),identity.end(),L'\0')-identity.begin())},serial);
            if(data)HidD_FreePreparsedData(data);CloseHandle(handle);
            if(match){selected=detail->DevicePath;++matches;}
        }
        SetupDiDestroyDeviceInfoList(devices);
        if(matches!=1){error=matches?"Ambiguous legacy CDU HID serial/collection":"Exact legacy CDU eight-byte HID collection unavailable";return false;}
        file_=CreateFileW(selected.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        reading_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);writing_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(file_==INVALID_HANDLE_VALUE || !reading_.hEvent || !writing_.hEvent || !HidD_SetNumInputBuffers(file_,512) || !HidD_FlushQueue(file_)){
            error="Cannot open/initialize legacy CDU HID queue";stop();return false;
        }
        // Runtime acquisition only: 64-contact input mode, then fresh snapshot.
        enabled_=true;
        if(!command(0x18,1) || !command(0x19)){error="Legacy CDU input enable/snapshot write failed";stop();return false;}
        error.clear();return true;
    }
    int read(std::array<std::uint8_t,8>& packet) noexcept override {
        if(file_==INVALID_HANDLE_VALUE)return -1;DWORD bytes{};
        if(!pending_){
            ResetEvent(reading_.hEvent);
            if(ReadFile(file_,buffer_.data(),static_cast<DWORD>(buffer_.size()),&bytes,&reading_)){
                if(bytes!=buffer_.size())return -1;packet=buffer_;return 1;
            }
            if(GetLastError()!=ERROR_IO_PENDING)return -1;pending_=true;
        }
        if(!GetOverlappedResult(file_,&reading_,&bytes,FALSE))return GetLastError()==ERROR_IO_INCOMPLETE?0:-1;
        pending_=false;if(bytes!=buffer_.size())return -1;packet=buffer_;return 1;
    }
    void stop() noexcept override {
        if(file_!=INVALID_HANDLE_VALUE){
            if(pending_){CancelIoEx(file_,&reading_);DWORD bytes{};GetOverlappedResult(file_,&reading_,&bytes,TRUE);pending_=false;}
            if(enabled_)command(0x18,0);enabled_=false;CloseHandle(file_);file_=INVALID_HANDLE_VALUE;
        }
        if(reading_.hEvent)CloseHandle(reading_.hEvent);if(writing_.hEvent)CloseHandle(writing_.hEvent);reading_={};writing_={};
    }
};
}
std::unique_ptr<Cdu3302Port> native_cdu3302_port(){return std::make_unique<NativePort>();}
} // namespace hwio::fds
