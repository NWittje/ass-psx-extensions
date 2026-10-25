#include "hwio/vendor_library.hpp"
#include <psx/windows/executable_path.hpp>
#include <Windows.h>
#include <msi.h>
#include <msiquery.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace hwio {
namespace {
std::wstring lower(std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;}
const wchar_t* filename(VendorLibrary kind){return kind==VendorLibrary::phidgets?L"phidget22.dll":L"interfaceITAPI x64.dll";}
bool x64(const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary);std::array<unsigned char,64> dos{};
    if(!in.read(reinterpret_cast<char*>(dos.data()),dos.size()) || dos[0]!='M' || dos[1]!='Z')return false;
    const auto offset=DWORD(dos[60])|(DWORD(dos[61])<<8)|(DWORD(dos[62])<<16)|(DWORD(dos[63])<<24);
    if(offset<64 || offset>16*1024*1024)return false;
    in.seekg(offset);std::array<unsigned char,6> pe{};
    return bool(in.read(reinterpret_cast<char*>(pe.data()),pe.size())) && pe[0]=='P' && pe[1]=='E' && !pe[2] && !pe[3] && pe[4]==0x64 && pe[5]==0x86;
}
struct MsiHandle {MSIHANDLE value{};~MsiHandle(){if(value)MsiCloseHandle(value);}};
std::wstring field(MSIHANDLE record,unsigned index){
    DWORD length{};wchar_t empty{};if(MsiRecordGetStringW(record,index,&empty,&length)!=ERROR_MORE_DATA && !length)return {};
    std::wstring value(length+1,L'\0');DWORD capacity=length+1;
    if(MsiRecordGetStringW(record,index,value.data(),&capacity)!=ERROR_SUCCESS)return {};
    value.resize(capacity);return value;
}
std::vector<std::filesystem::path> registered_libraries(VendorLibrary kind){
    std::vector<std::filesystem::path> out;
    // Read-only MSI queries use the installed component key path. In particular,
    // InstallLocation can be empty even for a correctly installed vendor SDK.
    for(DWORD index=0;index<4096;++index){
        wchar_t product[39]{};const auto next=MsiEnumProductsW(index,product);
        if(next==ERROR_NO_MORE_ITEMS)break;if(next!=ERROR_SUCCESS)continue;
        wchar_t name[1024]{};DWORD count=1024;
        if(MsiGetProductInfoW(product,INSTALLPROPERTY_INSTALLEDPRODUCTNAME,name,&count)!=ERROR_SUCCESS)continue;
        const auto title=lower(name);
        if(kind==VendorLibrary::phidgets?title.find(L"phidget22")==title.npos:title.find(L"interfaceit api")==title.npos)continue;
        std::array<wchar_t,32768> package{};count=static_cast<DWORD>(package.size());
        if(MsiGetProductInfoW(product,INSTALLPROPERTY_LOCALPACKAGE,package.data(),&count)!=ERROR_SUCCESS)continue;
        MsiHandle database,view;
        if(MsiOpenDatabaseW(package.data(),MSIDBOPEN_READONLY,&database.value)!=ERROR_SUCCESS)continue;
        const auto query=L"SELECT `File`.`FileName`, `Component`.`ComponentId` FROM `File`, `Component` WHERE `File`.`Component_` = `Component`.`Component`";
        if(MsiDatabaseOpenViewW(database.value,query,&view.value)!=ERROR_SUCCESS || MsiViewExecute(view.value,0)!=ERROR_SUCCESS)continue;
        for(unsigned row=0;row<16384;++row){
            MsiHandle record;if(MsiViewFetch(view.value,&record.value)!=ERROR_SUCCESS)break;
            auto file=field(record.value,1);const auto separator=file.find(L'|');if(separator!=file.npos)file.erase(0,separator+1);
            if(lower(file)!=lower(filename(kind)))continue;
            const auto component=field(record.value,2);if(component.empty())continue;
            std::array<wchar_t,32768> installed{};count=static_cast<DWORD>(installed.size());
            if(MsiGetComponentPathW(product,component.c_str(),installed.data(),&count)!=INSTALLSTATE_LOCAL)continue;
            const auto path=std::filesystem::path(installed.data());
            if(path.is_absolute())out.push_back(path.parent_path()/file);
        }
    }
    return out;
}
}
std::filesystem::path find_vendor_library(VendorLibrary kind,const std::wstring& selected,
        const std::filesystem::path& application,const std::vector<std::filesystem::path>& registered){
    std::vector<std::filesystem::path> paths;
    if(!selected.empty()){
        std::filesystem::path path(selected);if(!path.is_absolute())throw std::runtime_error("Choose an absolute vendor library folder in Settings > General");
        std::error_code error;if(std::filesystem::is_directory(path,error))path/=filename(kind);
        paths.push_back(std::move(path));
    }else{
        paths={application/L"libraries"/(kind==VendorLibrary::phidgets?L"phidgets":L"fds")/filename(kind),application/filename(kind)};
        paths.insert(paths.end(),registered.begin(),registered.end());
    }
    for(const auto& path:paths)if(path.is_absolute() && x64(path))return path;
    throw std::runtime_error(kind==VendorLibrary::phidgets?
        "Phidget22 x64 library unavailable. Open Settings > General > Phidgets > Choose folder.":
        "FDS x64 library unavailable. Open Settings > General > FDS > Choose folder.");
}
std::filesystem::path vendor_library(VendorLibrary kind,const std::wstring& selected){
    static std::once_flag phidgets_once,fds_once;static std::vector<std::filesystem::path> phidgets,fds;
    auto& paths=kind==VendorLibrary::phidgets?phidgets:fds;
    if(selected.empty())std::call_once(kind==VendorLibrary::phidgets?phidgets_once:fds_once,[&]{paths=registered_libraries(kind);});
    return find_vendor_library(kind,selected,psx::windows::executable_path().parent_path(),paths);
}
}
