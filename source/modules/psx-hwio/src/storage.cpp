#include <limits>
#include <psx/windows/executable_path.hpp>
#include <charconv>
#include "hwio/input_overrides.hpp"
#include "hwio/storage.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace hwio {
namespace {
constexpr std::size_t maximum = 32 * 1024 * 1024;
bool replace(const std::wstring &file, const std::wstring &temporary, std::string &error) {
    const bool exists = std::filesystem::exists(file);
    if (!(exists ? ReplaceFileW(file.c_str(), temporary.c_str(), (file + L".bak").c_str(), 0, nullptr, nullptr)
                 : MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH))) {
        error = "Cannot save " + utf8(file) + ": Windows error " + std::to_string(GetLastError());
        return false;
    }
    return true;
}
bool write(const std::wstring &file, const std::string &data, std::string &error) {
    const auto temporary = file + L".tmp";
    std::ofstream out(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) {
        error = "Cannot write " + utf8(file);
        return false;
    }
    out.close();
    return replace(file, temporary, error);
}
bool decode(const std::string &text, std::vector<Profile> &profiles, DisplayAssignments &display,
            AxisConfiguration &axes, std::string &error, ButtonConfiguration *buttons = nullptr, HardwareConfiguration *hardware = nullptr, CockpitConfiguration *cockpit = nullptr,ModuleConfiguration* modules=nullptr) {
    std::istringstream in(text);
    std::string header, encoded;
    std::size_t count{};
    std::getline(in, header);
    if (text.size() > maximum || (header != "[hwio-assignments-v1]" && header != "[hwio-assignments-v2]" && header != "[hwio-assignments-v3]" && header != "[hwio-assignments-v4]" && header!="[hwio-assignments-v5]" && header!="[hwio-assignments-v6]") || !(in >> count) || count > 256) {
        error = "Invalid assignments.cfg header or profile count";
        return false;
    }
    std::vector<Profile> parsed;
    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        Profile p;
        if (!(in >> std::quoted(encoded)) || !profile_decode(encoded, p, error) || !ids.insert(p.id).second) {
            if (error.empty())
                error = "Invalid or duplicate profile in assignments.cfg";
            return false;
        }
        parsed.push_back(std::move(p));
    }
    DisplayAssignments references;
    for (std::size_t i = 0; i < flight_slots; ++i) {
        std::size_t slot{};
        unsigned choice{};
        auto &r = references[i];
        if (!(in >> slot >> choice >> r.binding >> std::quoted(r.profile)) || slot != i || choice > 2 ||
            r.profile.size() > 1024 || r.profile.find_first_of("\r\n") != std::string::npos ||
            (choice == 2 && (r.profile.empty() || r.binding == 0))) {
            error = "Invalid display reference in assignments.cfg";
            return false;
        }
        r.choice = static_cast<DisplayChoice>(choice);
    }
    auto configuration=std::make_unique<AxisConfiguration>();
    if (!(in >> std::quoted(encoded)) || !axis_configuration_decode(encoded, *configuration, error)) {
        if (error.empty())
            error = "Missing axis configuration";
        return false;
    }
    ButtonConfiguration button_configuration;
    if (header != "[hwio-assignments-v1]" &&
        (!(in >> std::quoted(encoded)) || !button_configuration_decode(encoded, button_configuration, error))) {
        if (error.empty()) error = "Invalid button configuration";
        return false;
    }
    HardwareConfiguration hardware_configuration=initial_hardware_configuration();
    if((header=="[hwio-assignments-v3]" || header=="[hwio-assignments-v4]" || (header=="[hwio-assignments-v5]" || header=="[hwio-assignments-v6]")) && (!(in>>std::quoted(encoded)) || !hardware_decode(encoded,hardware_configuration,error)))return false;
    CockpitConfiguration cockpit_configuration;
    if((header=="[hwio-assignments-v4]" || (header=="[hwio-assignments-v5]" || header=="[hwio-assignments-v6]")) && (!(in>>std::quoted(encoded)) || !cockpit_decode(encoded,cockpit_configuration,error)))return false;
    ModuleConfiguration module_configuration;
    if((header=="[hwio-assignments-v5]" || header=="[hwio-assignments-v6]") && (!(in>>std::quoted(encoded)) || !modules_decode(encoded,module_configuration,error)))return false;
    in >> std::ws;
    if (!in.eof()) {
        error = "Unexpected assignments.cfg data";
        return false;
    }
    profiles = std::move(parsed);
    display = std::move(references);
    axes = std::move(*configuration);
    if (buttons) *buttons = std::move(button_configuration);
    if(hardware)*hardware=std::move(hardware_configuration);
    if(cockpit)*cockpit=std::move(cockpit_configuration);
    if(modules)*modules=std::move(module_configuration);
    return true;
}
std::string section(const std::wstring &file, const wchar_t *name) {
    wchar_t buffer[32768]{};
    const auto size = GetPrivateProfileSectionW(name, buffer, 32768, file.c_str());
    if (size >= 32766)
        throw std::runtime_error("Legacy settings section too large");
    std::string result = "[" + utf8(name) + "]\n";
    for (const wchar_t *p = buffer; *p; p += wcslen(p) + 1)
        result += utf8(p) + "\n";
    return result;
}
} // namespace
bool save_input_overrides(const std::wstring& directory,const InputOverrideConfiguration& configuration,std::string& error){
    return write(directory+L"\\input-overrides.cfg",input_overrides_encode(configuration),error);
}
std::wstring executable_directory() {
    return psx::windows::executable_path().parent_path().wstring();
}
bool load_assignments(const std::wstring &file, std::vector<Profile> &profiles, DisplayAssignments &display,
                      AxisConfiguration &axes, std::string &error, ButtonConfiguration *buttons, HardwareConfiguration *hardware, CockpitConfiguration *cockpit,ModuleConfiguration* modules) {
    try {
        if (std::filesystem::file_size(file) > maximum) {
            error = "assignments.cfg exceeds 32 MiB";
            return false;
        }
        std::ifstream in(std::filesystem::path(file), std::ios::binary);
        if (!in) {
            error = "Cannot read assignments.cfg";
            return false;
        }
        std::ostringstream data;
        data << in.rdbuf();
        return decode(data.str(), profiles, display, axes, error, buttons, hardware, cockpit,modules);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool save_assignments(const std::wstring &file, const std::vector<Profile> &profiles, const DisplayAssignments &display,
                      const AxisConfiguration &axes, std::string &error, const ButtonConfiguration *buttons, const HardwareConfiguration *hardware, const CockpitConfiguration *cockpit,const ModuleConfiguration* modules) {
    try {
        ButtonConfiguration retained;
        HardwareConfiguration retained_hardware=initial_hardware_configuration();
        CockpitConfiguration retained_cockpit;
        ModuleConfiguration retained_modules;
        if ((!buttons || !hardware || !cockpit || !modules) && std::filesystem::exists(file)) {
            std::vector<Profile> old_profiles;
            DisplayAssignments old_display;
            auto old_axes=std::make_unique<AxisConfiguration>();
            if (!load_assignments(file, old_profiles, old_display, *old_axes, error, &retained, &retained_hardware,&retained_cockpit,&retained_modules)) return false;
        }
        const auto &button_configuration = buttons ? *buttons : retained;
        const auto &hardware_configuration=hardware?*hardware:retained_hardware;
        const auto &cockpit_configuration=cockpit?*cockpit:retained_cockpit;
        const auto &module_configuration=modules?*modules:retained_modules;
        if(!modules_valid(module_configuration,error))return false;
        if(!cockpit_valid(cockpit_configuration,error))return false;
        if(!hardware_valid(hardware_configuration,error))return false;
        if (!button_configuration_valid(button_configuration, error)) return false;
        std::ostringstream out;
        out << "[hwio-assignments-v6]\n" << profiles.size() << '\n';
        for (const auto &p : profiles)
            out << std::quoted(profile_encode(p)) << '\n';
        for (std::size_t i = 0; i < flight_slots; ++i) {
            const auto &r = display[i];
            out << i << ' ' << static_cast<unsigned>(r.choice) << ' ' << r.binding << ' ' << std::quoted(r.profile)
                << '\n';
        }
        out << std::quoted(axis_configuration_encode(axes)) << '\n';
        out << std::quoted(button_configuration_encode(button_configuration)) << '\n';
        out << std::quoted(hardware_encode(hardware_configuration)) << '\n';
        out << std::quoted(cockpit_encode(cockpit_configuration)) << '\n';
        out << std::quoted(modules_encode(module_configuration)) << '\n';
        std::vector<Profile> checked_profiles;
        DisplayAssignments checked_display;
        auto checked_axes=std::make_unique<AxisConfiguration>();
        if (!decode(out.str(), checked_profiles, checked_display, *checked_axes, error))
            return false;
        return write(file, out.str(), error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool prepare_configuration(const std::wstring &directory, const std::wstring &legacy, std::string &error) {
    try {
        std::filesystem::create_directories(directory);
        const auto has_legacy = [](const std::wstring &dir) {
            return std::filesystem::exists(dir + L"\\axis-controls.ini") ||
                   std::filesystem::exists(dir + L"\\axis-display.ini") ||
                   std::filesystem::exists(dir + L"\\settings.ini") || std::filesystem::exists(dir + L"\\profiles");
        };
        const auto source = has_legacy(directory) || legacy.empty() ? directory : legacy;
        if (!std::filesystem::exists(directory + L"\\assignments.cfg")) {
            std::vector<Profile> profiles;
            DisplayAssignments display;
            AxisConfiguration axes;
            const auto folder = source + L"\\profiles";
            if (std::filesystem::exists(folder))
                for (const auto &entry : std::filesystem::directory_iterator(folder)) {
                    if (entry.path().extension() != L".ini")
                        continue;
                    Profile p;
                    if (!load_profile(entry.path().wstring(), p, error))
                        return false;
                    profiles.push_back(std::move(p));
                }
            if (!load_display_assignments(source + L"\\axis-display.ini", display, error) ||
                !load_axis_configuration(source + L"\\axis-controls.ini", axes, error))
                return false;
            for (std::size_t i = 9; i < 13; ++i) {
                auto &row = axes.rows[i];
                // Complete the user's previously requested switch, blocked by 1.3.0's mandatory reverser.
                if (axes.throttle_choice == 1 && row.pending == "Assign the separate reverser" &&
                    !row.draft.source.bindings.empty() &&
                    row.draft.source.bindings[0].throttle_layout == ThrottleLayout::separate_forward_reverse)
                    axis_apply(row, i);
            }
            if (!save_assignments(directory + L"\\assignments.cfg", profiles, display, axes, error))
                return false;
        }
        if (!std::filesystem::exists(directory + L"\\settings.cfg")) {
            const auto old = source + L"\\settings.ini";
            std::string data = section(old, L"main");
            if (data == "[main]\n")
                data += "host=127.0.0.1\nport=10747\n";
            data += section(source + L"\\diagnostics.ini", L"logging");
            data += section(source + L"\\diagnostics.ini", L"diagnostics");
            data += "[input]\nspike_smooth_ms=25\n";
            if (!write(directory + L"\\settings.cfg", data, error))
                return false;
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool save_endpoint(const std::wstring &directory, const std::string &host, std::uint16_t port, std::string &error) {
    try {
        const auto file = directory + L"\\settings.cfg", temporary = file + L".tmp";
        std::filesystem::copy_file(file, temporary, std::filesystem::copy_options::overwrite_existing);
        if (!WritePrivateProfileStringW(L"main", L"host", wide(host).c_str(), temporary.c_str()) ||
            !WritePrivateProfileStringW(L"main", L"port", std::to_wstring(port).c_str(), temporary.c_str())) {
            error = "Cannot write settings.cfg";
            return false;
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
        return replace(file, temporary, error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool save_button_navigation(const std::wstring &directory, int chapter, bool breakers, std::string &error, int view) {
    try {
        const auto file=directory+L"\\settings.cfg", temporary=file+L".tmp";
        std::filesystem::copy_file(file,temporary,std::filesystem::copy_options::overwrite_existing);
        if (!WritePrivateProfileStringW(L"button-pane",L"chapter",std::to_wstring(chapter).c_str(),temporary.c_str()) ||
            !WritePrivateProfileStringW(L"button-pane",L"breakers",breakers?L"1":L"0",temporary.c_str()) ||
            (view>=0&&!WritePrivateProfileStringW(L"button-pane",L"selected",view?L"1":L"0",temporary.c_str()))) {
            error="Cannot save button navigation"; return false;
        }
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temporary.c_str());
        return replace(file,temporary,error);
    } catch (const std::exception &e) {error=e.what();return false;}
}
AppSettings load_settings(const std::wstring &directory) {
    const auto file = directory + L"\\settings.cfg";
    AppSettings result;
    result.fdstq.enabled=GetPrivateProfileIntW(L"fdstq",L"enabled",0,file.c_str())!=0;
    result.fdstq.remote=GetPrivateProfileIntW(L"fdstq",L"remote",1,file.c_str())!=0;
    result.fdstq.paired=GetPrivateProfileIntW(L"fdstq",L"paired",1,file.c_str())!=0;
    result.fdstq.serial=static_cast<int>(GetPrivateProfileIntW(L"fdstq",L"serial",0,file.c_str()));
    result.fdstq.port=GetPrivateProfileIntW(L"fdstq",L"port",5661,file.c_str());
    result.fdstq.inverted_switches=GetPrivateProfileIntW(L"fdstq",L"inverted_switches",0,file.c_str());
    wchar_t tq_text[512]{};GetPrivateProfileStringW(L"fdstq",L"host",L"",tq_text,512,file.c_str());result.fdstq.host=utf8(tq_text);
    for(unsigned n=0;n<5;++n){GetPrivateProfileStringW(L"fdstq",(L"output"+std::to_wstring(n)).c_str(),L"",tq_text,512,file.c_str());result.fdstq.outputs[n]=utf8(tq_text);}
    auto tq_number=[&](const std::wstring& key,double fallback){const auto text=std::to_wstring(fallback);GetPrivateProfileStringW(L"fdstq",key.c_str(),text.c_str(),tq_text,512,file.c_str());wchar_t* end{};const double value=wcstod(tq_text,&end);return end==tq_text || *end?std::numeric_limits<double>::quiet_NaN():value;};
    result.fdstq.hub_serial=static_cast<int>(GetPrivateProfileIntW(L"fdstq",L"hub_serial",0,file.c_str()));result.fdstq.servo_serial=static_cast<int>(GetPrivateProfileIntW(L"fdstq",L"servo_serial",0,file.c_str()));result.fdstq.pair_follow=GetPrivateProfileIntW(L"fdstq",L"pair_follow",0,file.c_str());
    for(unsigned n=0;n<3;++n){auto& m=result.fdstq.steppers[n];const auto key=L"stepper"+std::to_wstring(n)+L"_";m.enabled=GetPrivateProfileIntW(L"fdstq",(key+L"enabled").c_str(),0,file.c_str())!=0;m.reversed=GetPrivateProfileIntW(L"fdstq",(key+L"reversed").c_str(),0,file.c_str())!=0;m.current=tq_number(key+L"current",m.current);m.acceleration=tq_number(key+L"acceleration",0);m.velocity=tq_number(key+L"velocity",0);}
    result.fdstq.trim_enabled=GetPrivateProfileIntW(L"fdstq",L"trim_enabled",0,file.c_str())!=0;result.fdstq.servo_enabled=GetPrivateProfileIntW(L"fdstq",L"servo_enabled",0,file.c_str())!=0;
    result.fdstq.trim_counts_per_degree=tq_number(L"trim_counts_per_degree",0);result.fdstq.trim_velocity=tq_number(L"trim_velocity",0);result.fdstq.trim_acceleration=tq_number(L"trim_acceleration",.2);
    result.fdstq.servo_zero=tq_number(L"servo_zero",0);result.fdstq.servo_full=tq_number(L"servo_full",0);result.fdstq.servo_velocity=tq_number(L"servo_velocity",0);result.fdstq.servo_acceleration=tq_number(L"servo_acceleration",0);
    wchar_t adapter[128]{};
    GetPrivateProfileStringW(L"hardware-network",L"interface_guid",L"",adapter,128,file.c_str());
    result.network_interface=adapter;
    wchar_t api[32768]{};
    GetPrivateProfileStringW(L"fds",L"api_path",L"",api,32768,file.c_str());result.fds_api_path=api;
    GetPrivateProfileStringW(L"phidgets",L"api_path",L"",api,32768,file.c_str());result.phidget_api_path=api;
    result.provider_selection_initialized=GetPrivateProfileIntW(L"providers",L"initialized",0,file.c_str())!=0;
    result.pokeys_enabled=GetPrivateProfileIntW(L"providers",L"pokeys",1,file.c_str())!=0;
    result.fds_enabled=GetPrivateProfileIntW(L"providers",L"fds",!result.fds_api_path.empty(),file.c_str())!=0;
    result.phidgets_enabled=GetPrivateProfileIntW(L"providers",L"phidgets",0,file.c_str())!=0;
    result.trim_displays.enabled=GetPrivateProfileIntW(L"providers",L"ass_trim",0,file.c_str())!=0;
    result.cpflight.enabled=GetPrivateProfileIntW(L"cpflight",L"enabled",0,file.c_str())!=0;
    result.flightillusion_enabled=GetPrivateProfileIntW(L"flightillusion",L"enabled",0,file.c_str())!=0;
    {auto read=[&](const wchar_t* key){wchar_t text[2049]{};GetPrivateProfileStringW(L"cpflight",key,L"",text,2049,file.c_str());return std::wstring(text);};auto& c=result.cpflight;c.port.com_name=read(L"port");c.port.instance_id=read(L"instance");c.port.interface_path=read(L"interface");c.port.friendly_name=read(L"name");c.serial=utf8(read(L"serial"));std::string reason;if(!valid_cpflight_configuration(c,reason)){c.enabled=false;c.port={};c.serial.clear();}}
    for(unsigned n=0;n<2;++n){
        auto& b=result.trim_displays.roles[n];const auto section=n?L"ass-trim-fo":L"ass-trim-cpt";
        auto read=[&](const wchar_t* key){wchar_t text[2049]{};const auto length=GetPrivateProfileStringW(section,key,L"",text,2049,file.c_str());if(length>=2048)b.device=UINT64_MAX;return std::wstring(text);};
        const auto id=utf8(read(L"identity"));if(!id.empty()){const auto parsed=std::from_chars(id.data(),id.data()+id.size(),b.device);if(parsed.ec!=std::errc{} || parsed.ptr!=id.data()+id.size())b.device=UINT64_MAX;}
        b.port.com_name=read(L"port");b.port.instance_id=read(L"instance");b.port.interface_path=read(L"interface");b.port.friendly_name=read(L"name");
    }
    result.network_selection_initialized=GetPrivateProfileIntW(L"hardware-network",L"selected",0,file.c_str())!=0;
    wchar_t level[32]{};
    GetPrivateProfileStringW(L"logging", L"level", L"", level, 32, file.c_str());
    if (_wcsicmp(level, L"off") == 0 || (!level[0] && !GetPrivateProfileIntW(L"logging", L"enabled", 1, file.c_str())))
        result.log_level = LogLevel::off;
    else if (_wcsicmp(level, L"warn") == 0)
        result.log_level = LogLevel::warn;
    const auto ms = GetPrivateProfileIntW(L"input", L"spike_smooth_ms", 25, file.c_str());
    if (ms == 10 || ms == 25 || ms == 50)
        result.smoothing_ms = static_cast<int>(ms);
    return result;
}
bool save_settings(const std::wstring &directory, const AppSettings &settings, std::string &error) {
    if(!fdstq_valid(settings.fdstq,error))return false;
    if(!valid_trim_configuration(settings.trim_displays,error) || !valid_cpflight_configuration(settings.cpflight,error))return false;
    if ((settings.smoothing_ms != 10 && settings.smoothing_ms != 25 && settings.smoothing_ms != 50) ||
        (settings.log_level != LogLevel::off && settings.log_level != LogLevel::info &&
         settings.log_level != LogLevel::warn)) {
        error = "Choose Off, Info or Warn and 10, 25 or 50 ms";
        return false;
    }
    try {
        const auto file = directory + L"\\settings.cfg", temporary = file + L".tmp";
        std::filesystem::create_directories(directory);
        if(std::filesystem::exists(file))std::filesystem::copy_file(file, temporary, std::filesystem::copy_options::overwrite_existing);
        else {std::ofstream fresh(std::filesystem::path(temporary),std::ios::binary|std::ios::trunc);if(!fresh){error="Cannot create settings.cfg";return false;}}
        const auto level = settings.log_level == LogLevel::off    ? L"off"
                           : settings.log_level == LogLevel::warn ? L"warn"
                                                                  : L"info";
        if (!WritePrivateProfileStringW(L"fds",L"api_path",settings.fds_api_path.c_str(),temporary.c_str()) ||
            !WritePrivateProfileStringW(L"phidgets",L"api_path",settings.phidget_api_path.c_str(),temporary.c_str()) ||
            !WritePrivateProfileStringW(L"providers",L"initialized",L"1",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"providers",L"pokeys",settings.pokeys_enabled?L"1":L"0",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"providers",L"fds",settings.fds_enabled?L"1":L"0",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"providers",L"phidgets",settings.phidgets_enabled?L"1":L"0",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"providers",L"ass_trim",settings.trim_displays.enabled?L"1":L"0",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"hardware-network",L"interface_guid",settings.network_interface.c_str(),temporary.c_str()) ||
            !WritePrivateProfileStringW(L"hardware-network",L"selected",settings.network_selection_initialized?L"1":L"0",temporary.c_str()) ||
            !WritePrivateProfileStringW(L"logging", L"level", level, temporary.c_str()) ||
            !WritePrivateProfileStringW(L"logging", L"enabled", settings.log_level == LogLevel::off ? L"0" : L"1",
                                        temporary.c_str()) ||
            !WritePrivateProfileStringW(L"input", L"spike_smooth_ms", std::to_wstring(settings.smoothing_ms).c_str(),
                                        temporary.c_str())) {
            error = "Cannot write settings.cfg";
            return false;
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
        {const auto& c=settings.cpflight;auto put=[&](const wchar_t* key,const std::wstring& text){return WritePrivateProfileStringW(L"cpflight",key,text.c_str(),temporary.c_str())!=0;};if(!put(L"enabled",c.enabled?L"1":L"0") || !put(L"port",c.port.com_name) || !put(L"instance",c.port.instance_id) || !put(L"interface",c.port.interface_path) || !put(L"name",c.port.friendly_name) || !put(L"serial",wide(c.serial))){error="Cannot save CPflight connection";return false;}}
        if(!WritePrivateProfileStringW(L"flightillusion",L"enabled",settings.flightillusion_enabled?L"1":L"0",temporary.c_str())){error="Cannot save Flight Illusion connection";return false;}
        const auto& tq=settings.fdstq;
        for(unsigned n=0;n<2;++n){const auto& b=settings.trim_displays.roles[n];const auto section=n?L"ass-trim-fo":L"ass-trim-cpt";
            if(!WritePrivateProfileStringW(section,L"identity",std::to_wstring(b.device).c_str(),temporary.c_str()) ||
               !WritePrivateProfileStringW(section,L"port",b.port.com_name.c_str(),temporary.c_str()) ||
               !WritePrivateProfileStringW(section,L"instance",b.port.instance_id.c_str(),temporary.c_str()) ||
               !WritePrivateProfileStringW(section,L"interface",b.port.interface_path.c_str(),temporary.c_str()) ||
               !WritePrivateProfileStringW(section,L"name",b.port.friendly_name.c_str(),temporary.c_str())){error="Cannot save trim-display assignments";return false;}
        }
        if(!WritePrivateProfileStringW(L"fdstq",L"enabled",tq.enabled?L"1":L"0",temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"remote",tq.remote?L"1":L"0",temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"paired",tq.paired?L"1":L"0",temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"serial",std::to_wstring(tq.serial).c_str(),temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"host",wide(tq.host).c_str(),temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"port",std::to_wstring(tq.port).c_str(),temporary.c_str()) ||
           !WritePrivateProfileStringW(L"fdstq",L"inverted_switches",std::to_wstring(tq.inverted_switches).c_str(),temporary.c_str())){error="Cannot save FDSTQ settings";return false;}
        for(unsigned n=0;n<5;++n)if(!WritePrivateProfileStringW(L"fdstq",(L"output"+std::to_wstring(n)).c_str(),wide(tq.outputs[n]).c_str(),temporary.c_str())){error="Cannot save FDSTQ output assignment";return false;}
        auto tq_write=[&](const std::wstring& key,const std::wstring& value){if(!WritePrivateProfileStringW(L"fdstq",key.c_str(),value.c_str(),temporary.c_str()))throw std::runtime_error("Cannot save FDSTQ motor settings");};
        auto tq_double=[&](const std::wstring& key,double value){std::wostringstream text;text.precision(17);text<<value;tq_write(key,text.str());};
        tq_write(L"hub_serial",std::to_wstring(tq.hub_serial));tq_write(L"servo_serial",std::to_wstring(tq.servo_serial));tq_write(L"pair_follow",std::to_wstring(tq.pair_follow));
        for(unsigned n=0;n<3;++n){const auto& m=tq.steppers[n];const auto key=L"stepper"+std::to_wstring(n)+L"_";tq_write(key+L"enabled",m.enabled?L"1":L"0");tq_write(key+L"reversed",m.reversed?L"1":L"0");tq_double(key+L"current",m.current);tq_double(key+L"acceleration",m.acceleration);tq_double(key+L"velocity",m.velocity);}
        tq_write(L"trim_enabled",tq.trim_enabled?L"1":L"0");tq_write(L"servo_enabled",tq.servo_enabled?L"1":L"0");tq_double(L"trim_counts_per_degree",tq.trim_counts_per_degree);tq_double(L"trim_velocity",tq.trim_velocity);tq_double(L"trim_acceleration",tq.trim_acceleration);
        tq_double(L"servo_zero",tq.servo_zero);tq_double(L"servo_full",tq.servo_full);tq_double(L"servo_velocity",tq.servo_velocity);tq_double(L"servo_acceleration",tq.servo_acceleration);
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temporary.c_str());
        return replace(file, temporary, error);
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace hwio
