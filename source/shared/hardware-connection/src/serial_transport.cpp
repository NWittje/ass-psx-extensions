#include "transport.hpp"
#include <algorithm>
#include <stdexcept>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <ntddser.h>

namespace psx::hardware {
namespace {
std::wstring property(HDEVINFO set,SP_DEVINFO_DATA& device,const DEVPROPKEY& key){
    std::array<wchar_t,512> buffer{};DEVPROPTYPE type{};DWORD needed{};
    if(!SetupDiGetDevicePropertyW(set,&device,&key,&type,reinterpret_cast<PBYTE>(buffer.data()),DWORD(sizeof(buffer)),&needed,0)||type!=DEVPROP_TYPE_STRING)return {};
    buffer.back()=0;return buffer.data();
}
}
std::vector<SerialPort> enumerate_serial_ports(){
    std::vector<SerialPort> result;
    auto set=SetupDiGetClassDevsW(&GUID_DEVINTERFACE_COMPORT,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return result;
    struct DeviceSet {HDEVINFO value;~DeviceSet(){SetupDiDestroyDeviceInfoList(value);}} cleanup{set};
    for(DWORD index=0;;++index){
        SP_DEVICE_INTERFACE_DATA face{};face.cbSize=sizeof(face);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&GUID_DEVINTERFACE_COMPORT,index,&face))break;
        DWORD needed{};SetupDiGetDeviceInterfaceDetailW(set,&face,nullptr,0,&needed,nullptr);
        if(!needed||needed>65536)continue;
        std::vector<std::uint8_t> storage(needed);auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize=sizeof(*detail);SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&face,detail,needed,nullptr,&device))continue;
        auto key=SetupDiOpenDevRegKey(set,&device,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);
        if(key==INVALID_HANDLE_VALUE)continue;
        std::array<wchar_t,64> port{};DWORD type{},bytes=DWORD(sizeof(port));
        auto error=RegQueryValueExW(key,L"PortName",nullptr,&type,reinterpret_cast<BYTE*>(port.data()),&bytes);RegCloseKey(key);
        if(error!=ERROR_SUCCESS||type!=REG_SZ||bytes<sizeof(wchar_t)||bytes>sizeof(port))continue;
        port.back()=0;const std::wstring port_name=port.data();
        if(!port_name.starts_with(L"COM")||port_name.size()<4||
           !std::all_of(port_name.begin()+3,port_name.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';}))continue;
        std::array<wchar_t,512> instance{};
        SetupDiGetDeviceInstanceIdW(set,&device,instance.data(),DWORD(instance.size()),nullptr);
        auto friendly=property(set,device,DEVPKEY_Device_FriendlyName);
        if(friendly.empty())friendly=property(set,device,DEVPKEY_Device_DeviceDesc);
        result.push_back({port_name,std::move(friendly),detail->DevicePath,instance.data()});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
        auto number=[](const std::wstring& name){return name.size()>3?wcstoul(name.c_str()+3,nullptr,10):0ul;};
        const auto x=number(a.com_name),y=number(b.com_name);return x==y?a.com_name<b.com_name:x<y;
    });
    return result;
}
}
namespace psx::hardware::detail {
SerialTransport::SerialTransport(){
    reading_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    writing_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!reading_.hEvent||!writing_.hEvent){
        if(reading_.hEvent)CloseHandle(reading_.hEvent);
        if(writing_.hEvent)CloseHandle(writing_.hEvent);
        throw std::runtime_error("Cannot create serial completion events");
    }
}
SerialTransport::~SerialTransport(){cancel_close();CloseHandle(reading_.hEvent);CloseHandle(writing_.hEvent);}
bool SerialTransport::open(const SerialPort& port,DWORD& error) noexcept {
    // Use the enumerated device interface, not a guessed COM-number alias.
    file_=CreateFileW(port.interface_path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(file_==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    DCB dcb{};dcb.DCBlength=sizeof(dcb);
    if(!GetCommState(file_,&dcb)){error=GetLastError();cancel_close();return false;}
    dcb.BaudRate=CBR_115200;dcb.ByteSize=8;dcb.Parity=NOPARITY;dcb.StopBits=ONESTOPBIT;
    dcb.fBinary=TRUE;dcb.fParity=FALSE;dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;
    dcb.fDtrControl=DTR_CONTROL_DISABLE;dcb.fDsrSensitivity=FALSE;dcb.fTXContinueOnXoff=TRUE;
    dcb.fOutX=FALSE;dcb.fInX=FALSE;dcb.fErrorChar=FALSE;dcb.fNull=FALSE;
    dcb.fRtsControl=RTS_CONTROL_DISABLE;dcb.fAbortOnError=FALSE;
    COMMTIMEOUTS timeouts{MAXDWORD,MAXDWORD,50,0,1000};
    if(!SetCommState(file_,&dcb)||!SetCommTimeouts(file_,&timeouts)){
        error=GetLastError();cancel_close();return false;
    }
    return true;
}
IoResult SerialTransport::read(std::span<std::uint8_t> destination) noexcept {
    DWORD count{};
    if(!read_pending_){
        ResetEvent(reading_.hEvent);
        if(!ReadFile(file_,input_.data(),DWORD(std::min(input_.size(),destination.size())),nullptr,&reading_)){
            const auto error=GetLastError();if(error==ERROR_IO_PENDING){read_pending_=true;return {};}
            return {IoState::failed,0,error};
        }
        read_pending_=true;
    }
    // An OVERLAPPED handle reports its actual transfer count through completion, even on immediate success.
    if(!GetOverlappedResult(file_,&reading_,&count,FALSE)){
        const auto error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return {};
        read_pending_=false;return {IoState::failed,0,error};
    }
    read_pending_=false;
    if(count>destination.size())return {IoState::failed,0,ERROR_INVALID_DATA};
    std::copy_n(input_.begin(),count,destination.begin());return {IoState::completed,count,0};
}
IoResult SerialTransport::write(Bytes source) noexcept {
    DWORD count{};
    if(!write_pending_){
        if(source.empty()||source.size()>output_.size())return {IoState::failed,0,ERROR_INVALID_PARAMETER};
        std::copy(source.begin(),source.end(),output_.begin());ResetEvent(writing_.hEvent);
        if(!WriteFile(file_,output_.data(),DWORD(source.size()),nullptr,&writing_)){
            const auto error=GetLastError();if(error==ERROR_IO_PENDING){write_pending_=true;return {};}
            return {IoState::failed,0,error};
        }
        write_pending_=true;
    }
    if(!GetOverlappedResult(file_,&writing_,&count,FALSE)){
        const auto error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return {};
        write_pending_=false;return {IoState::failed,0,error};
    }
    write_pending_=false;
    return count?IoResult{IoState::completed,count,0}:IoResult{IoState::failed,0,ERROR_WRITE_FAULT};
}
void SerialTransport::wait(HANDLE wake,DWORD milliseconds) noexcept {
    HANDLE handles[3]{wake,reading_.hEvent,writing_.hEvent};DWORD count=1;
    if(read_pending_)handles[count++]=reading_.hEvent;
    if(write_pending_)handles[count++]=writing_.hEvent;
    WaitForMultipleObjects(count,handles,FALSE,milliseconds);
}
void SerialTransport::cancel_close() noexcept {
    if(file_==INVALID_HANDLE_VALUE)return;
    CancelIoEx(file_,nullptr);
    // Cancellation is only a request. Both buffers and OVERLAPPED records stay alive through completion.
    DWORD ignored{};
    if(read_pending_)GetOverlappedResult(file_,&reading_,&ignored,TRUE);
    if(write_pending_)GetOverlappedResult(file_,&writing_,&ignored,TRUE);
    read_pending_=write_pending_=false;CloseHandle(file_);file_=INVALID_HANDLE_VALUE;
}
TransportOps SerialTransport::operations() noexcept {
    return {this,
        [](void* c,const SerialPort& p,DWORD& e) noexcept{return static_cast<SerialTransport*>(c)->open(p,e);},
        [](void* c,std::span<std::uint8_t> b) noexcept{return static_cast<SerialTransport*>(c)->read(b);},
        [](void* c,Bytes b) noexcept{return static_cast<SerialTransport*>(c)->write(b);},
        [](void* c,HANDLE w,DWORD ms) noexcept{static_cast<SerialTransport*>(c)->wait(w,ms);},
        [](void* c) noexcept{static_cast<SerialTransport*>(c)->cancel_close();}};
}
}
