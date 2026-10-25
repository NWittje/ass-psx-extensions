#include <cmath>
#include <du_client/config.hpp>
#include <psx/network/io.hpp>
#include <windows.h>
#include <ws2tcpip.h>
#include <sddl.h>
namespace du_client
{
std::filesystem::path executable_directory()
{
    std::wstring s(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, s.data(), DWORD(s.size()));
    if (!n || n >= s.size())
        throw std::runtime_error("Executable path unavailable");
    s.resize(n);
    return std::filesystem::path(s).parent_path();
}
std::wstring profile_name(std::wstring_view s)
{
    if (s == L"10" || (s.size() == 1 && s[0] >= L'1' && s[0] <= L'9'))
        return std::wstring(s);
    throw std::runtime_error("Profile number must be 1..10");
}
std::wstring user_identity()
{
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("User token unavailable");
    DWORD n{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> b(n);
    auto ok = GetTokenInformation(token, TokenUser, b.data(), n, &n);
    CloseHandle(token);
    if (!ok)
        throw std::runtime_error("User identity unavailable");
    LPWSTR s{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &s))
        throw std::runtime_error("User SID unavailable");
    std::wstring out = s;
    LocalFree(s);
    return out;
}
std::string network_host(std::string_view value)
{
    return psx::network::host_ipv4(value);
}
void validate(const Config &c)
{
    (void)network_host(c.host);
    if(c.adapter_guid.size()>64)throw std::runtime_error("Invalid adapter identity");
    if(!c.tcp_port)throw std::runtime_error("DU host port must be 1..65535");
    if (c.placement.size != sizeof c.placement || c.placement.version != 1 || c.placement.mode > 2 ||
        c.placement.dpi < 48 || c.placement.dpi > 960 || int64_t(c.placement.right) - c.placement.left <= 0 ||
        int64_t(c.placement.bottom) - c.placement.top <= 0 || std::abs(int64_t(c.x)) > 65536 ||
        std::abs(int64_t(c.y)) > 65536 || c.width < 0 || c.width > 16384 || c.unit >= 6 ||
        std::abs(int64_t(c.trim.x)) > 65536 || std::abs(int64_t(c.trim.y)) > 65536 ||
        c.trim.width < 0 || c.trim.width > 16384)
        throw std::runtime_error("Invalid DU window settings");
}
Loaded SettingsStore::load() const
{
    Loaded r;
    if (profile_ < 1 || profile_ > 10)
    {
        r.error = L"Profile number must be 1..10";
        return r;
    }
    r.missing = !std::filesystem::exists(path_);
    if (r.missing)
    {
        r.valid = true;
        return r;
    }
    try
    {
        ass::graphics::Settings s(path_.c_str());
        s.load();
        if (s.get(L"client", L"schema") != L"2" && s.get(L"client",L"schema")!=L"3" && s.get(L"client",L"schema")!=L"4" && s.get(L"client",L"schema")!=L"5" && s.get(L"client",L"schema")!=L"6")
            throw std::runtime_error("Unsupported profile schema");
        if (profile_ < 1 || profile_ > 10)
            throw std::runtime_error("Profile number must be 1..10");
        const auto section = L"profile." + std::to_wstring(profile_);
        auto get = [&](const wchar_t *key)
        {
            uint32_t n{};
            auto result = ag_settings_get(s.handle(), section.c_str(), key, nullptr, 0, &n);
            if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                return std::wstring{};
            ass::graphics::check(result);
            return s.get(section.c_str(), key);
        };
        if (get(L"present").empty())
        {
            r.missing = true;
            r.valid = true;
            return r;
        }
        if (get(L"present") != L"1")
            throw std::runtime_error("Invalid profile marker");
        auto &c = r.config;
        auto number = [&](const wchar_t *key, double fallback, double lo, double hi)
        {
            auto text = get(key);
            if (text.empty())
                return fallback;
            size_t used = 0;
            double value = std::stod(text, &used);
            if (used != text.size() || !std::isfinite(value) || value != std::floor(value) || value < lo ||
                value > hi)
                throw std::runtime_error("Invalid numeric profile value");
            return value;
        };
        if(s.get(L"client",L"schema")==L"2")c.network=false;
        else {
            auto mode=get(L"source_mode");if(!mode.empty()&&mode!=L"network"&&mode!=L"tcp"&&mode!=L"local")throw std::runtime_error("Invalid source mode");
            c.network=mode!=L"local";auto host=get(L"host_ipv4");if(!host.empty()){c.host.clear();for(auto ch:host){if(ch>127)throw std::runtime_error("Host must be ASCII IPv4");c.host.push_back(char(ch));}}c.adapter_guid=get(L"adapter_guid");
        }
        if(s.get(L"client",L"schema")==L"4" || s.get(L"client",L"schema")==L"5" || s.get(L"client",L"schema")==L"6")c.tcp_port=uint16_t(number(L"tcp_port",47480,1,65535));
        else if(c.network)r.warning=L"Legacy network profile now uses TCP; settings remain unchanged until Save.";
        c.unit = unsigned(number(L"du", 0, 0, 5));
        c.placement.left = int32_t(number(L"left", 100, -65536, 65536));
        c.placement.top = int32_t(number(L"top", 100, -65536, 65536));
        c.placement.right = int32_t(number(L"right", 900, -65536, 65536));
        c.placement.bottom = int32_t(number(L"bottom", 700, -65536, 65536));
        c.placement.dpi = uint32_t(number(L"dpi", 96, 48, 960));
        c.placement.mode = uint32_t(number(L"mode", 0, 0, 2));
        auto monitor = get(L"monitor");
        if (monitor.size() >= 128)
            throw std::runtime_error("Invalid monitor identity");
        wcscpy_s(c.placement.monitor, monitor.c_str());
        c.x = int(number(L"alignment_x", 0, -65536, 65536));
        c.y = int(number(L"alignment_y", 0, -65536, 65536));
        c.width = int(number(L"alignment_width", 0, 0, 16384));
        c.fullscreen_x = int(number(L"fullscreen_x", 0, -65536, 65536));
        c.fullscreen_y = int(number(L"fullscreen_y", 0, -65536, 65536));
        c.anchor = s.boolean(section.c_str(), L"fullscreen_anchor", false);
        if ((s.get(L"client", L"schema") == L"5" || s.get(L"client", L"schema") == L"6"))
        {
            c.trim.enabled = s.boolean(section.c_str(), L"trim_enabled", false);
            c.trim.always_show = s.boolean(section.c_str(), L"trim_always_show", false);
            c.trim.x = int(number(L"trim_alignment_x", 0, -65536, 65536));
            c.trim.y = int(number(L"trim_alignment_y", 0, -65536, 65536));
            c.trim.width = int(number(L"trim_alignment_width", 0, 0, 16384));
        }
        // Legacy raster preferences are intentionally ignored; never rewrite on load.
        validate(c);
        if(s.get(L"client",L"schema")!=L"6" && c.network)r.warning=L"Network adapter selection is now automatic; Save profile to persist the new edition.";
        r.valid = true;
    }
    catch (const std::exception &e)
    {
        r.config = Config{};
        r.error = L"Cannot load DU profile: ";
        for (auto p = e.what(); *p; p++)
            r.error.push_back(wchar_t(uint8_t(*p)));
    }
    return r;
}
void SettingsStore::save(const Config &c) const
{
    save(c, profile_);
}
void SettingsStore::save(const Config &c, unsigned profile) const
{
    validate(c);
    if (profile < 1 || profile > 10)
        throw std::runtime_error("Profile number must be 1..10");
    // Serialize read/merge/atomic replacement across independently running profiles.
    auto identity = std::filesystem::absolute(path_).lexically_normal().wstring();
    uint64_t hash = 14695981039346656037ull;
    for (auto ch : identity)
    {
        hash ^= uint16_t(towlower(ch));
        hash *= 1099511628211ull;
    }
    auto name = L"Local\\PSX-IOS.DuProfiles." + std::to_wstring(hash);
    struct Lock
    {
        HANDLE handle{};
        bool owned{};
        ~Lock()
        {
            if (owned)
                ReleaseMutex(handle);
            if (handle)
                CloseHandle(handle);
        }
    } lock;
    lock.handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!lock.handle)
        throw std::runtime_error("Profile lock unavailable");
    const auto waited = WaitForSingleObject(lock.handle, 5000);
    lock.owned = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    if (!lock.owned)
        throw std::runtime_error("Profile file is busy; save again");
    const auto section = L"profile." + std::to_wstring(profile);
    std::filesystem::create_directories(path_.parent_path());
    ass::graphics::Settings s(path_.c_str());
    if (std::filesystem::exists(path_))
    {
        s.load();
        const auto schema=s.get(L"client",L"schema");
        if(schema!=L"2"&&schema!=L"3"&&schema!=L"4"&&schema!=L"5"&&schema!=L"6")throw std::runtime_error("Unsupported profile schema");
        // Schema 2 was local-only. Upgrade every preserved numbered profile,
        // so saving one slot cannot change the source of the other nine slots.
        if(schema==L"2")for(unsigned n=1;n<=10;++n){const auto preserved=L"profile."+std::to_wstring(n);uint32_t length{};if(ag_settings_get(s.handle(),preserved.c_str(),L"present",nullptr,0,&length)==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))continue;if(s.get(preserved.c_str(),L"present")==L"1")s.set(preserved.c_str(),L"source_mode",L"local");}
    }
    s.set(L"client", L"schema", L"6");
    s.set(section.c_str(), L"present", L"1");
    s.number(section.c_str(), L"du", c.unit);
    s.set(section.c_str(),L"source_mode",c.network?L"tcp":L"local");
    s.set(section.c_str(),L"tcp_port",std::to_wstring(c.tcp_port).c_str());
    s.set(section.c_str(),L"host_ipv4",std::wstring(c.host.begin(),c.host.end()).c_str());
    s.set(section.c_str(),L"adapter_guid",c.adapter_guid.c_str());
    auto num = [&](const wchar_t *k, double v) { s.number(section.c_str(), k, v); };
    num(L"left", c.placement.left);
    num(L"top", c.placement.top);
    num(L"right", c.placement.right);
    num(L"bottom", c.placement.bottom);
    num(L"dpi", c.placement.dpi);
    num(L"mode", c.placement.mode);
    s.set(section.c_str(), L"monitor", c.placement.monitor);
    num(L"alignment_x", c.x);
    num(L"alignment_y", c.y);
    num(L"alignment_width", c.width);
    num(L"fullscreen_x", c.fullscreen_x);
    num(L"fullscreen_y", c.fullscreen_y);
    s.set_boolean(section.c_str(), L"fullscreen_anchor", c.anchor);
    s.set_boolean(section.c_str(), L"trim_enabled", c.trim.enabled);
    s.set_boolean(section.c_str(), L"trim_always_show", c.trim.always_show);
    num(L"trim_alignment_x", c.trim.x);
    num(L"trim_alignment_y", c.trim.y);
    num(L"trim_alignment_width", c.trim.width);
    s.set_boolean(section.c_str(), L"fixed_native_raster", false);
    s.save();
}
} // namespace du_client
