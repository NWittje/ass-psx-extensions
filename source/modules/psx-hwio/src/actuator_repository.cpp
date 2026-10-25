#include "hwio/actuator_repository.hpp"
#include "hwio/motor_mapping.hpp"
#include "hwio/servo.hpp"
#include "hwio/actuators.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
namespace hwio {
namespace {
std::recursive_mutex repository_mutex;
using Sections=std::map<std::string,std::string>;
std::optional<std::string> read(const std::filesystem::path& p){
    if(!std::filesystem::exists(p))return {};
    if(std::filesystem::file_size(p)>1048576)throw std::runtime_error("Actuator file exceeds 1 MiB");
    std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("Cannot read actuator file");
    return std::string(std::istreambuf_iterator<char>(in),{});
}
bool key(std::string_view name){return name=="motors" || name=="servos" || name=="gauges";}
Sections parse(std::string_view text){
    std::istringstream in{std::string(text)};std::string header;std::getline(in,header);if(!header.empty() && header.back()=='\r')header.pop_back();
    if(header!="[hwio-actuator-mappings-v1]" && header!="[hwio-actuator-mappings-v2]")throw std::runtime_error("Invalid authoritative actuator-mappings.cfg header");
    Sections result;std::string name,value;while(in>>std::quoted(name)){
        if(!key(name) || result.contains(name) || !(in>>std::quoted(value)) || value.size()>524288)throw std::runtime_error("Invalid actuator repository section");result.emplace(name,std::move(value));
    }
    if(!in.eof() || !result.contains("motors") || !result.contains("servos") || !result.contains("gauges"))throw std::runtime_error("Incomplete authoritative actuator repository");
    return result;
}
bool write(const std::wstring& dir,const Sections& sections,std::string& error,bool migration=false){
    std::ostringstream text;text<<"[hwio-actuator-mappings-v2]\n";for(const auto& [name,value]:sections)text<<std::quoted(name)<<' '<<std::quoted(value)<<'\n';
    return actuator_atomic_write((std::filesystem::path(dir)/L"actuator-mappings.cfg").wstring(),text.str(),error,migration?L".pre-analogue-layout-20261019.bak":L".bak");
}
}
bool actuator_atomic_write(const std::wstring& path,std::string_view text,std::string& error,const wchar_t* backup_suffix){
    try{std::lock_guard guard(repository_mutex);if(text.size()>1048576)throw std::runtime_error("Actuator configuration exceeds 1 MiB");
        const std::filesystem::path p(path);const auto temporary=path+L".tmp",backup=path+backup_suffix;
        if(std::wstring_view(backup_suffix)!=L".bak" && std::filesystem::exists(backup))throw std::runtime_error("Numbering migration backup already exists; previous files retained");
        std::ofstream out(std::filesystem::path(temporary),std::ios::binary|std::ios::trunc);out.write(text.data(),std::streamsize(text.size()));out.flush();if(!out)throw std::runtime_error("Cannot stage actuator configuration");out.close();
        if(!(std::filesystem::exists(p)?ReplaceFileW(path.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr):MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)))throw std::runtime_error("Cannot commit actuator configuration; Windows error "+std::to_string(GetLastError()));error.clear();return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool actuator_section(const std::wstring& dir,std::string_view name,std::wstring_view legacy,std::optional<std::string>& value,std::string& error){
    try{std::lock_guard guard(repository_mutex);if(!key(name))throw std::runtime_error("Unknown actuator section");
        const auto file=read(std::filesystem::path(dir)/L"actuator-mappings.cfg");if(file){auto sections=parse(*file);value=sections.at(std::string(name));if(value->empty())value.reset();}
        else value=legacy.empty()?std::nullopt:read(std::filesystem::path(dir)/legacy);
        error.clear();return true;
    }catch(const std::exception& e){value.reset();error=e.what();return false;}
}
bool prepare_actuator_repository(const std::wstring& dir,std::string& error){
    try{std::lock_guard guard(repository_mutex);const auto path=std::filesystem::path(dir)/L"actuator-mappings.cfg";
        const auto existing=read(path);if(existing){(void)parse(*existing);if(existing->starts_with("[hwio-actuator-mappings-v2]")){error.clear();return true;}}
        const auto motor_pre=read(std::filesystem::path(dir)/L"motor-mappings.cfg"),servo_pre=read(std::filesystem::path(dir)/L"servo-settings.cfg");
        MotorMappingDefinitions motors;MotorAlignments alignments;std::array<ServoDefinition,servo_slots> servos;std::array<ActuatorMapping,actuator_mapping_slots> gauges;
        if(!load_motor_mappings(dir,motors,error,&alignments) || !load_servo_settings(dir,servos,error) || !load_actuator_mappings(dir,gauges,error))return false;
        if(motor_pre!=read(std::filesystem::path(dir)/L"motor-mappings.cfg") || servo_pre!=read(std::filesystem::path(dir)/L"servo-settings.cfg"))throw std::runtime_error("Legacy actuator configuration changed during migration");
        if(existing!=read(path))throw std::runtime_error("Actuator configuration changed during numbering migration");
        Sections sections=existing?parse(*existing):Sections{{"motors",motor_pre.value_or("")},{"servos",servo_pre.value_or("")},{"gauges",""}};
        std::array<bool,30> used{};
        for(auto& d:motors)if(d.enabled){unsigned slot{};while(slot<10 && used[slot])++slot;if(slot==10)throw std::runtime_error("More than ten analogue-feedback assignments; existing configuration retained");used[slot]=true;d.analogue_output=slot;}
        for(auto& d:gauges)if(d.enabled){const unsigned base=d.calibration==ActuatorCalibration::potentiometer?0:d.calibration==ActuatorCalibration::switches?10:20;unsigned slot=base;while(slot<base+10 && used[slot])++slot;if(slot==base+10)throw std::runtime_error("More than ten outputs in an analogue mapping group; existing configuration retained");used[slot]=true;d.analogue_output=slot;}
        if(!sections["motors"].empty())sections["motors"]=motor_mappings_encode(motors,alignments);
        if(!sections["gauges"].empty()){sections["gauges"]=actuator_mappings_encode(gauges,error);if(sections["gauges"].empty())return false;}
        return write(dir,sections,error,true);
    }catch(const std::exception& e){error=e.what();return false;}
}
bool actuator_section_save(const std::wstring& dir,std::string_view name,std::string_view value,std::string& error){
    try{std::lock_guard guard(repository_mutex);if(!key(name) || value.size()>524288)throw std::runtime_error("Invalid actuator section write");
        if(!prepare_actuator_repository(dir,error))return false;const auto text=read(std::filesystem::path(dir)/L"actuator-mappings.cfg");if(!text)throw std::runtime_error("Actuator repository disappeared");auto sections=parse(*text);sections[std::string(name)]=value;return write(dir,sections,error);
    }catch(const std::exception& e){error=e.what();return false;}
}
}
