#include "hwio/catalogue.hpp"
#include "hwio/core.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <cmath>

namespace hwio {
std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                nullptr, nullptr);
    if (!n)
        return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
std::wstring wide(std::string_view text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!n)
        return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::wstring profile_directory() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        out = path;
        CoTaskMemFree(path);
    }
    return out + L"\\PSX-IOS\\HWIO";
}
static std::string escape(std::string_view text) {
    std::string out;
    const char *hex = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if (c < 32 || c == '%' || c == '=' || c == '[' || c == ']') {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out;
}
static bool unescape(std::string_view text, std::string &out) {
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    out.clear();
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size())
                return false;
            int a = digit(text[++i]), b = digit(text[++i]);
            if (a < 0 || b < 0)
                return false;
            c = static_cast<char>((a << 4) | b);
        }
        if (c == 0 || c == '\r' || c == '\n')
            return false;
        out += c;
    }
    return out.empty() || !wide(out).empty();
}
static std::string source_string(ControlKey k) {
    if(k.backend!=BackendKind::usb_hid) return "net,"+std::to_string(static_cast<unsigned>(k.backend))+","+std::to_string(static_cast<unsigned>(k.kind))+","+std::to_string(k.channel_kind)+","+std::to_string(k.channel);
    return std::to_string(static_cast<int>(k.kind)) + "," + std::to_string(k.usage_page) + "," +
           std::to_string(k.usage) + "," + std::to_string(k.link_collection) + "," + std::to_string(k.report_id) + "," +
           std::to_string(k.data_index);
}
std::string profile_encode(const Profile &p) {
    std::ostringstream s;
    s << std::setprecision(17);
    auto line = [&](const char *key, const auto &value) { s << key << '=' << value << '\n'; };
    s << "; PSX HWIO UTF-8 profile. Percent escapes protect delimiters. No device-order identity.\n[profile]\n";
    line("revision", profile_revision);
    line("id", escape(p.id));
    line("name", escape(p.name));
    line("explicit_path", p.explicit_path);
    const auto &d = p.identity;
    s << "[device]\n";
    line("vendor", d.vendor);
    line("product", d.product);
    line("revision", d.revision);
    line("usage_page", d.usage_page);
    line("usage", d.usage);
    line("serial", escape(d.serial));
    line("container", escape(d.container_id));
    line("name", escape(d.name));
    line("descriptor", escape(d.descriptor_signature));
    line("collection", escape(d.collection_identity));
    line("instance", escape(d.instance_path));
    if(d.backend!=BackendKind::usb_hid){line("backend",static_cast<unsigned>(d.backend));line("hardware_key",escape(d.hardware_key));}
    for (const auto &b : p.bindings) {
        s << "[binding." << b.id << "]\n";
        line("name", escape(b.name));
        line("enabled", b.enabled);
        line("mode", static_cast<int>(b.mode));
        line("throttle", static_cast<int>(b.throttle));
        line("step", b.step);
        line("throttle_layout", static_cast<int>(b.throttle_layout));
        line("reverse_source", source_string(b.reverse_source.key));
        line("reverse_profile", escape(b.reverse_source.profile));
        const auto &rc = b.reverse_calibration;
        line("reverse_minimum", rc.minimum);
        line("reverse_center", rc.center);
        line("reverse_maximum", rc.maximum);
        line("reverse_dead_center_low", rc.dead_center_low);
        line("reverse_dead_center_high", rc.dead_center_high);
        line("reverse_dead_minimum", rc.dead_minimum);
        line("reverse_dead_maximum", rc.dead_maximum);
        line("reverse_centered", rc.centered);
        line("reverse_invert", rc.inverted);
        line("reverse_median_three", rc.median_three);
        line("reverse_lowpass_ms", rc.lowpass_ms);
        const auto &inactive = b.inactive_calibration;
        s << "inactive_calibration=" << inactive.minimum << ',' << inactive.center << ',' << inactive.maximum << ','
          << inactive.dead_center_low << ',' << inactive.dead_center_high << ',' << inactive.dead_minimum << ','
          << inactive.dead_maximum << ',' << inactive.lowpass_ms << ',' << inactive.centered << ',' << inactive.inverted
          << ',' << inactive.median_three << '\n';
        line("inactive_source", source_string(b.inactive_source.key));
        s << "sources=";
        for (std::size_t i = 0; i < b.sources.size(); ++i) {
            if (i)
                s << '|';
            s << source_string(b.sources[i].key);
        }
        s << '\n';
        s << "selector=";
        bool first = true;
        for (auto item : b.selector) {
            if (!first)
                s << ',';
            s << item.first << ':' << item.second;
            first = false;
        }
        s << '\n';
        s << "thresholds=";
        for (std::size_t i = 0; i < b.thresholds.size(); ++i) {
            if (i)
                s << ',';
            s << b.thresholds[i];
        }
        s << '\n';
        line("hysteresis", b.hysteresis);
        const auto &c = b.calibration;
        line("minimum", c.minimum);
        line("center", c.center);
        line("maximum", c.maximum);
        line("dead_center_low", c.dead_center_low);
        line("dead_center_high", c.dead_center_high);
        line("dead_minimum", c.dead_minimum);
        line("dead_maximum", c.dead_maximum);
        line("centered", c.centered);
        line("invert", c.inverted);
        line("median_three", c.median_three);
        line("lowpass_ms", c.lowpass_ms);
        line("control_stiffness", b.control_stiffness);
        const auto &t = b.target;
        line("target_id", escape(t.id));
        line("target_name", escape(t.name));
        line("unit", escape(t.unit));
        line("record", record_name(t.record));
        line("layout", static_cast<int>(t.layout));
        line("semantics", static_cast<int>(t.semantics));
        line("target_minimum", t.minimum);
        line("target_maximum", t.maximum);
        line("released", t.released);
        line("pressed", t.pressed);
        line("mask", t.owned_mask);
        line("transient", t.transient_mask);
        line("shift", static_cast<int>(t.shift));
        line("field", t.field);
        line("field_count", t.field_count);
        line("engine", static_cast<int>(t.throttle_engine));
        line("writable", t.writable);
    }
    return s.str();
}
using Section = std::map<std::string, std::string>;
static std::vector<std::string> split(const std::string &text, char delim) {
    std::vector<std::string> out;
    if (text.empty())
        return out;
    std::istringstream s(text);
    std::string part;
    while (std::getline(s, part, delim))
        out.push_back(part);
    return out;
}
bool profile_decode(std::string_view text, Profile &result, std::string &error) {
    try {
        if (text.size() > 1024 * 1024)
            throw std::runtime_error("Profile exceeds 1 MiB");
        std::map<std::string, Section> ini;
        std::string section;
        std::istringstream input{std::string(text)};
        std::string line;
        bool firstline = true;
        while (std::getline(input, line)) {
            if (firstline && line.substr(0, 3) == "\xEF\xBB\xBF")
                line.erase(0, 3);
            firstline = false;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                if (ini.count(section))
                    throw std::runtime_error("Duplicate section");
                ini[section];
                continue;
            }
            auto eq = line.find('=');
            if (section.empty() || eq == line.npos)
                throw std::runtime_error("Invalid INI line");
            const auto key = line.substr(0, eq);
            if (!ini[section].emplace(key, line.substr(eq + 1)).second)
                throw std::runtime_error("Duplicate key");
        }
        auto str = [](const Section &s, const char *key) {
            auto it = s.find(key);
            if (it == s.end())
                throw std::runtime_error(std::string("Missing key: ") + key);
            return it->second;
        };
        auto num = [&](const Section &s, const char *key) -> long long {
            auto v = str(s, key);
            std::size_t n = 0;
            auto x = std::stoll(v, &n);
            if (n != v.size())
                throw std::runtime_error("Invalid integer");
            return x;
        };
        auto real = [&](const Section &s, const char *key) {
            auto v = str(s, key);
            std::size_t n = 0;
            auto x = std::stod(v, &n);
            if (n != v.size() || !std::isfinite(x))
                throw std::runtime_error("Invalid number");
            return x;
        };
        auto txt = [&](const Section &s, const char *key) {
            std::string v;
            if (!unescape(str(s, key), v))
                throw std::runtime_error("Invalid UTF-8 or percent encoding");
            return v;
        };
        auto bounded = [&](const Section &s, const char *key, long long lo, long long hi) {
            auto n = num(s, key);
            if (n < lo || n > hi)
                throw std::runtime_error(std::string("Out of range: ") + key);
            return n;
        };
        auto boolean = [&](const Section &s, const char *key) { return bounded(s, key, 0, 1) != 0; };
        Profile p;
        const auto &head = ini.at("profile");
        if (num(head, "revision") != profile_revision)
            throw std::runtime_error("Unsupported profile revision");
        p.id = txt(head, "id");
        p.name = txt(head, "name");
        if (head.count("explicit_path"))
            p.explicit_path = boolean(head, "explicit_path");
        const auto &dev = ini.at("device");
        auto &d = p.identity;
        d.vendor = static_cast<std::uint16_t>(bounded(dev, "vendor", 0, 65535));
        d.product = static_cast<std::uint16_t>(bounded(dev, "product", 0, 65535));
        d.revision = static_cast<std::uint16_t>(bounded(dev, "revision", 0, 65535));
        d.usage_page = static_cast<std::uint16_t>(bounded(dev, "usage_page", 0, 65535));
        d.usage = static_cast<std::uint16_t>(bounded(dev, "usage", 0, 65535));
        d.serial = txt(dev, "serial");
        d.container_id = txt(dev, "container");
        d.name = txt(dev, "name");
        d.descriptor_signature = txt(dev, "descriptor");
        d.collection_identity = txt(dev, "collection");
        d.instance_path = txt(dev, "instance");
        if(dev.count("backend")){d.backend=static_cast<BackendKind>(bounded(dev,"backend",0,5));d.hardware_key=txt(dev,"hardware_key");}
        for (const auto &entry : ini) {
            if (entry.first == "profile" || entry.first == "device")
                continue;
            if (entry.first.compare(0, 8, "binding.") != 0)
                throw std::runtime_error("Unknown profile section");
            Binding b;
            const auto id = entry.first.substr(8);
            std::size_t end = 0;
            b.id = std::stoull(id, &end);
            if (end != id.size())
                throw std::runtime_error("Invalid binding ID");
            const auto &s = entry.second;
            b.name = txt(s, "name");
            b.enabled = boolean(s, "enabled");
            b.mode = static_cast<MappingMode>(bounded(s, "mode", 0, 5));
            b.throttle = static_cast<ThrottlePolicy>(bounded(s, "throttle", 0, 1));
            b.step = static_cast<int>(bounded(s, "step", -100000, 100000));
            auto read_source = [&](std::string_view source) {
                auto a=split(std::string(source), ','); ControlKey k;
                if(a.size()==5 && a[0]=="net") {
                    int v[4]{};for(std::size_t i=0;i<4;++i)if(!integer(a[i+1],v[i]) || v[i]<0 || v[i]>65535)throw std::runtime_error("Invalid network source key");
                    if(v[0]<1 || v[0]>5 || v[1]>2 || v[2]==0)throw std::runtime_error("Invalid provider source namespace");
                    k.backend=static_cast<BackendKind>(v[0]);k.kind=static_cast<ControlKind>(v[1]);k.channel_kind=static_cast<std::uint16_t>(v[2]);k.channel=static_cast<std::uint16_t>(v[3]);return k;
                }
                if(a.size()!=6)throw std::runtime_error("Invalid source key field count");
                int v[6]{};for(std::size_t i=0;i<6;++i)if(!integer(a[i],v[i]) || v[i]<0 || v[i]>65535)throw std::runtime_error("Invalid source key");
                if(v[0]>2 || v[4]>255)throw std::runtime_error("Invalid source kind/report");
                k.kind=static_cast<ControlKind>(v[0]);k.usage_page=static_cast<std::uint16_t>(v[1]);k.usage=static_cast<std::uint16_t>(v[2]);k.link_collection=static_cast<std::uint16_t>(v[3]);k.report_id=static_cast<std::uint8_t>(v[4]);k.data_index=static_cast<std::uint16_t>(v[5]);return k;
            };
            for(auto source:split(str(s,"sources"),'|'))b.sources.push_back({read_source(source)});
            if (s.count("throttle_layout")) {
                b.throttle_layout = static_cast<ThrottleLayout>(bounded(s, "throttle_layout", 0, 1));
                b.reverse_source.key=read_source(str(s,"reverse_source"));
                auto &rc = b.reverse_calibration;
                rc.minimum = real(s, "reverse_minimum");
                rc.center = real(s, "reverse_center");
                rc.maximum = real(s, "reverse_maximum");
                rc.dead_center_low = real(s, "reverse_dead_center_low");
                rc.dead_center_high = real(s, "reverse_dead_center_high");
                rc.dead_minimum = real(s, "reverse_dead_minimum");
                rc.dead_maximum = real(s, "reverse_dead_maximum");
                rc.centered = boolean(s, "reverse_centered");
                rc.inverted = boolean(s, "reverse_invert");
                rc.median_three = boolean(s, "reverse_median_three");
                rc.lowpass_ms = real(s, "reverse_lowpass_ms");
            }
            if (s.count("reverse_profile"))
                b.reverse_source.profile = txt(s, "reverse_profile");
            if (s.count("inactive_calibration")) {
                auto a = split(str(s, "inactive_calibration"), ',');
                if (a.size() != 11)
                    throw std::runtime_error("Invalid inactive calibration");
                double v[11]{};
                for (std::size_t i = 0; i < a.size(); ++i) {
                    std::size_t n = 0;
                    v[i] = std::stod(a[i], &n);
                    if (n != a[i].size() || !std::isfinite(v[i]))
                        throw std::runtime_error("Invalid inactive calibration value");
                }
                auto &c = b.inactive_calibration;
                c.minimum = v[0];
                c.center = v[1];
                c.maximum = v[2];
                c.dead_center_low = v[3];
                c.dead_center_high = v[4];
                c.dead_minimum = v[5];
                c.dead_maximum = v[6];
                c.lowpass_ms = v[7];
                c.centered = v[8] != 0;
                c.inverted = v[9] != 0;
                c.median_three = v[10] != 0;
            }
            if(s.count("inactive_source"))b.inactive_source.key=read_source(str(s,"inactive_source"));
            for (auto pair : split(str(s, "selector"), ',')) {
                auto a = split(pair, ':');
                int x{}, y{};
                if (a.size() != 2 || !integer(a[0], x) || !integer(a[1], y) || !b.selector.emplace(x, y).second)
                    throw std::runtime_error("Invalid/duplicate selector pattern");
            }
            for (auto v : split(str(s, "thresholds"), ',')) {
                std::size_t n = 0;
                double x = std::stod(v, &n);
                if (n != v.size() || !std::isfinite(x))
                    throw std::runtime_error("Invalid selector threshold");
                b.thresholds.push_back(x);
            }
            b.hysteresis = real(s, "hysteresis");
            auto &c = b.calibration;
            c.minimum = real(s, "minimum");
            c.center = real(s, "center");
            c.maximum = real(s, "maximum");
            c.dead_center_low = real(s, "dead_center_low");
            c.dead_center_high = real(s, "dead_center_high");
            c.dead_minimum = real(s, "dead_minimum");
            c.dead_maximum = real(s, "dead_maximum");
            c.centered = boolean(s, "centered");
            c.inverted = boolean(s, "invert");
            c.median_three = boolean(s, "median_three");
            c.lowpass_ms = real(s, "lowpass_ms");
            b.control_stiffness = s.count("control_stiffness") ? real(s, "control_stiffness") : 0;
            b.response_gain = std::pow(4., b.control_stiffness);
            auto &t = b.target;
            t.id = txt(s, "target_id");
            t.name = txt(s, "target_name");
            t.unit = txt(s, "unit");
            if (!parse_key(str(s, "record"), t.record))
                throw std::runtime_error("Invalid Q record");
            t.layout = static_cast<ValueLayout>(bounded(s, "layout", 0, 3));
            t.semantics = static_cast<Semantics>(bounded(s, "semantics", 0, 4));
            t.minimum = static_cast<int>(bounded(s, "target_minimum", INT32_MIN, INT32_MAX));
            t.maximum = static_cast<int>(bounded(s, "target_maximum", INT32_MIN, INT32_MAX));
            t.released = static_cast<int>(bounded(s, "released", INT32_MIN, INT32_MAX));
            t.pressed = static_cast<int>(bounded(s, "pressed", INT32_MIN, INT32_MAX));
            t.owned_mask = static_cast<std::uint32_t>(bounded(s, "mask", 0, UINT32_MAX));
            t.transient_mask = static_cast<std::uint32_t>(bounded(s, "transient", 0, UINT32_MAX));
            t.shift = static_cast<std::uint8_t>(bounded(s, "shift", 0, 31));
            t.field = static_cast<std::uint16_t>(bounded(s, "field", 0, 255));
            t.field_count = static_cast<std::uint16_t>(bounded(s, "field_count", 0, 256));
            t.throttle_engine = static_cast<std::int8_t>(bounded(s, "engine", -1, 3));
            t.writable = boolean(s, "writable");
            resolve_catalogue_target(b.target);
            p.bindings.push_back(std::move(b));
        }
        if (!validate(p, error))
            return false;
        result = std::move(p);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool save_profile(const std::wstring &path, const Profile &p, std::string &error) {
    if (!validate(p, error))
        return false;
    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    const auto data = profile_encode(p);
    const auto temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot create profile temporary file: " + std::to_string(GetLastError());
        return false;
    }
    DWORD written{};
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temp.c_str());
        error = "Profile write/flush failed";
        return false;
    }
    bool replaced = false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        bool primary_valid = false;
        std::ifstream previous(std::filesystem::path(path), std::ios::binary | std::ios::ate);
        if (previous && previous.tellg() >= 0 && previous.tellg() <= 1024 * 1024) {
            previous.seekg(0);
            std::string contents((std::istreambuf_iterator<char>(previous)), {});
            Profile parsed;
            std::string ignored;
            primary_valid = profile_decode(contents, parsed, ignored);
        }
        previous.close();
        replaced = ReplaceFileW(path.c_str(), temp.c_str(), primary_valid ? (path + L".bak").c_str() : nullptr, 0,
                                nullptr, nullptr) != FALSE;
    } else
        replaced = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) {
        error = "Atomic profile replacement failed: " + std::to_string(GetLastError());
        DeleteFileW(temp.c_str());
    }
    return replaced;
}
bool load_profile(const std::wstring &path, Profile &p, std::string &error, bool *recovered) {
    if (recovered)
        *recovered = false;
    auto read = [&](const std::wstring &file) {
        std::ifstream in(std::filesystem::path(file), std::ios::binary | std::ios::ate);
        if (!in) {
            error = "Cannot open profile";
            return false;
        }
        const auto length = in.tellg();
        if (length < 0 || length > 1024 * 1024) {
            error = "Profile exceeds 1 MiB";
            return false;
        }
        in.seekg(0);
        std::string data((std::istreambuf_iterator<char>(in)), {});
        return profile_decode(data, p, error);
    };
    if (read(path))
        return true;
    const auto primary_error = error;
    if (read(path + L".bak")) {
        if (recovered)
            *recovered = true;
        error = "Recovered backup after: " + primary_error;
        return true;
    }
    error = primary_error;
    return false;
}
} // namespace hwio
