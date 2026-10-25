#include "coordinator/crash_policy.hpp"
#include <psx/diagnostics/journal.hpp>
#include <Windows.h>
#include <filesystem>
#include <array>
#include <algorithm>
namespace coordinator {
namespace {
constexpr wchar_t key[]=L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\psx-xp-coordinator.exe";
constexpr wchar_t owner[]=L"PSX-IOS coordinator diagnostics v1";
const std::array<const wchar_t*,4> names{L"DumpFolder",L"DumpType",L"DumpCount",L"PSX-IOS-Owner"};
bool value(const RegistryRecord& r,const wchar_t* name,DWORD type,DWORD number,const std::wstring& text={}) {
    const auto it=r.values.find(name);return it!=r.values.end() && it->second.type==type &&
        (type==REG_DWORD?it->second.number==number:it->second.text==text);
}
bool managed(const RegistryRecord& r){return value(r,L"PSX-IOS-Owner",REG_SZ,0,owner);}
bool settings(const RegistryRecord& r,const std::wstring& folder){return value(r,L"DumpFolder",REG_EXPAND_SZ,0,folder) && value(r,L"DumpType",REG_DWORD,1) && value(r,L"DumpCount",REG_DWORD,5);}
}
std::wstring crash_dump_directory(){return psx::diagnostics::data_directory()+L"\\Dumps\\Coordinator";}
CrashPolicyStatus inspect_crash_policy(CrashRegistry& registry,bool desired,const std::wstring& folder) {
    const auto r=registry.read();CrashPolicyStatus s;s.desired=desired;s.error=r.error;s.owned=managed(r);
    s.key_exists=r.exists;
    const auto type=r.values.find(L"DumpType");if(type!=r.values.end()&&type->second.type==REG_DWORD){s.type_present=true;s.actual_type=type->second.number;}
    const auto count=r.values.find(L"DumpCount");if(count!=r.values.end()&&count->second.type==REG_DWORD){s.count_present=true;s.actual_count=count->second.number;}
    const auto i=r.values.find(L"DumpFolder");if(i!=r.values.end())s.folder=i->second.text;
    s.effective=settings(r,folder);s.conflict=r.exists && (!s.owned || !s.effective);
    s.needs_setup=desired?(!s.effective || !s.owned):s.owned;return s;
}
std::uint32_t apply_crash_policy(CrashRegistry& registry,bool enable,const std::wstring& folder) {
    if(folder.empty() || !std::filesystem::path(folder).is_absolute())return ERROR_INVALID_PARAMETER;
    const auto r=registry.read();if(r.error)return r.error;
    if(r.exists && (!managed(r) || !settings(r,folder)))return ERROR_ALREADY_EXISTS;
    if(enable) {
        if(r.exists)return ERROR_SUCCESS;
        // Owner is committed last. Partial setup remains a visible conflict, never silently adopted.
        const std::array<RegistryValue,4> values{{{REG_EXPAND_SZ,0,folder},{REG_DWORD,1,{}},{REG_DWORD,5,{}},{REG_SZ,0,owner}}};
        for(std::size_t i=0;i<names.size();++i){const auto error=registry.put(names[i],values[i]);if(error)return error;}
    } else if(r.exists) {
        for(const auto* name:names){const auto error=registry.erase(name);if(error)return error;}
        return registry.erase_empty_key(); // Never deletes other values/subkeys.
    }
    return ERROR_SUCCESS;
}
RegistryRecord WindowsCrashRegistry::read() {
    RegistryRecord result;HKEY handle=nullptr;const auto opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,key,0,KEY_READ|KEY_WOW64_64KEY,&handle);
    if(opened==ERROR_FILE_NOT_FOUND)return result;if(opened!=ERROR_SUCCESS){result.error=opened;return result;}result.exists=true;
    for(const auto* name:names){DWORD type=0,size=0;auto error=RegQueryValueExW(handle,name,nullptr,&type,nullptr,&size);if(error==ERROR_FILE_NOT_FOUND)continue;
        if(error || size>32768){result.error=error?error:ERROR_INVALID_DATA;break;}
        std::wstring buffer((size+sizeof(wchar_t)-1)/sizeof(wchar_t)+1,L'\0');
        error=RegQueryValueExW(handle,name,nullptr,&type,reinterpret_cast<BYTE*>(buffer.data()),&size);if(error){result.error=error;break;}
        RegistryValue v;v.type=type;if(type==REG_DWORD && size==sizeof(DWORD))memcpy(&v.number,buffer.data(),sizeof(DWORD));
        else if((type==REG_SZ || type==REG_EXPAND_SZ) && size%sizeof(wchar_t)==0 && size>=sizeof(wchar_t) && buffer[size/sizeof(wchar_t)-1]==L'\0')v.text=buffer.c_str();
        else v.type=REG_NONE;result.values.emplace(name,std::move(v));}
    RegCloseKey(handle);return result;
}
std::uint32_t WindowsCrashRegistry::put(const std::wstring& name,const RegistryValue& v) {
    if(std::find(names.begin(),names.end(),name)==names.end())return ERROR_INVALID_PARAMETER;
    HKEY handle=nullptr;auto error=RegCreateKeyExW(HKEY_LOCAL_MACHINE,key,0,nullptr,0,KEY_SET_VALUE|KEY_WOW64_64KEY,nullptr,&handle,nullptr);if(error)return error;
    error=RegSetValueExW(handle,name.c_str(),0,v.type,v.type==REG_DWORD?reinterpret_cast<const BYTE*>(&v.number):reinterpret_cast<const BYTE*>(v.text.c_str()),v.type==REG_DWORD?sizeof(DWORD):static_cast<DWORD>((v.text.size()+1)*sizeof(wchar_t)));RegCloseKey(handle);return error;
}
std::uint32_t WindowsCrashRegistry::erase(const std::wstring& name) {
    if(std::find(names.begin(),names.end(),name)==names.end())return ERROR_INVALID_PARAMETER;
    HKEY handle=nullptr;auto error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,key,0,KEY_SET_VALUE|KEY_WOW64_64KEY,&handle);if(error)return error;
    error=RegDeleteValueW(handle,name.c_str());RegCloseKey(handle);return error==ERROR_FILE_NOT_FOUND?ERROR_SUCCESS:error;
}
std::uint32_t WindowsCrashRegistry::erase_empty_key() {
    HKEY handle=nullptr;auto error=RegOpenKeyExW(HKEY_LOCAL_MACHINE,key,0,KEY_READ|KEY_WOW64_64KEY,&handle);if(error)return error;
    DWORD subkeys=0,values=0;error=RegQueryInfoKeyW(handle,nullptr,nullptr,nullptr,&subkeys,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);RegCloseKey(handle);
    if(error || subkeys || values)return error;return RegDeleteKeyExW(HKEY_LOCAL_MACHINE,key,KEY_WOW64_64KEY,0);
}
}
