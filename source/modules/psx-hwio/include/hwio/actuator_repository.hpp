#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace hwio {
// One authoritative atomic file. Legacy files are read only before migration;
// an invalid authoritative file never falls back to potentially live old maps.
bool actuator_section(const std::wstring&,std::string_view,std::wstring_view,std::optional<std::string>&,std::string&);
bool actuator_section_save(const std::wstring&,std::string_view,std::string_view,std::string&);
bool prepare_actuator_repository(const std::wstring&,std::string&);
bool actuator_atomic_write(const std::wstring&,std::string_view,std::string&,const wchar_t* backup_suffix=L".bak");
}
