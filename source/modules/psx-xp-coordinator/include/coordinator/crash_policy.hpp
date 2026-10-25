#pragma once
#include <cstdint>
#include <map>
#include <string>
namespace coordinator {
struct RegistryValue {std::uint32_t type{},number{};std::wstring text;};
struct RegistryRecord {bool exists{};std::uint32_t error{};std::map<std::wstring,RegistryValue> values;};
class CrashRegistry {
public:
    virtual ~CrashRegistry()=default;
    virtual RegistryRecord read()=0;
    virtual std::uint32_t put(const std::wstring&,const RegistryValue&)=0;
    virtual std::uint32_t erase(const std::wstring&)=0;
    virtual std::uint32_t erase_empty_key()=0;
};
struct CrashPolicyStatus {bool desired{},effective{},owned{},conflict{},needs_setup{},key_exists{};std::uint32_t error{},actual_type{},actual_count{};bool type_present{},count_present{};std::wstring folder;};
CrashPolicyStatus inspect_crash_policy(CrashRegistry&,bool desired,const std::wstring& folder);
// Only the explicitly elevated helper calls this mutator. Tests use an in-memory registry.
std::uint32_t apply_crash_policy(CrashRegistry&,bool enable,const std::wstring& folder);
class WindowsCrashRegistry final:public CrashRegistry {
public:
    RegistryRecord read() override;
    std::uint32_t put(const std::wstring&,const RegistryValue&) override;
    std::uint32_t erase(const std::wstring&) override;
    std::uint32_t erase_empty_key() override;
};
std::wstring crash_dump_directory();
}
