#include "hwio/cpflight.hpp"

namespace hwio {
namespace {
class NativeCpflightPort final:public CpflightPort {
    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool failure(std::string& error){error="Serial error "+std::to_string(GetLastError());return false;}
public:
    ~NativeCpflightPort() override{close();}
    bool open(const psx::hardware::SerialPort& p,std::string& error) override {
        close();handle_=CreateFileW((L"\\\\.\\"+p.com_name).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(handle_==INVALID_HANDLE_VALUE){const auto code=GetLastError();error=code==ERROR_ACCESS_DENIED?"Port in use or access denied":"Port unavailable ("+std::to_string(code)+")";return false;}
        DCB d{};d.DCBlength=sizeof(d);if(!GetCommState(handle_,&d))return failure(error);
        d.BaudRate=38400;d.ByteSize=8;d.Parity=NOPARITY;d.StopBits=ONESTOPBIT;d.fBinary=TRUE;d.fParity=FALSE;
        d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_ENABLE;d.fDsrSensitivity=FALSE;d.fOutX=FALSE;d.fInX=FALSE;d.fNull=FALSE;d.fAbortOnError=FALSE;d.fRtsControl=RTS_CONTROL_ENABLE;
        COMMTIMEOUTS t{};t.ReadIntervalTimeout=MAXDWORD;t.ReadTotalTimeoutConstant=20;t.WriteTotalTimeoutConstant=200;
        if(!SetCommState(handle_,&d) || !SetCommTimeouts(handle_,&t) || !SetupComm(handle_,4096,4096))return failure(error);
        return true;
    }
    int read(std::span<std::uint8_t> bytes,std::string& error) override {DWORD n{};if(!ReadFile(handle_,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)){failure(error);return -1;}return static_cast<int>(n);}
    bool write(std::string_view bytes,std::string& error) override {
        while(!bytes.empty()){DWORD n{};if(!WriteFile(handle_,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr))return failure(error);if(!n){error="Serial write timed out";return false;}bytes.remove_prefix(n);}return true;
    }
    void close() noexcept override {if(handle_!=INVALID_HANDLE_VALUE){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}}
};
}
std::unique_ptr<CpflightPort> native_cpflight_port(){return std::make_unique<NativeCpflightPort>();}
}
