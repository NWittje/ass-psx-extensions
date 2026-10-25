#include <cdu_client/config.hpp>
#include <cdu_client/shader.hpp>
#include <charconv>
#include <algorithm>
#include <cmath>
#include <objbase.h>
#include <stdexcept>
#include <cwctype>
namespace cdu_client
{
std::wstring float_text(float v)
{
    char text[64];
    auto [end, e] = std::to_chars(text, text + 64, v);
    if (e != std::errc{})
        throw std::runtime_error("Cannot format shader parameter");
    return widen(std::string_view(text, end - text));
}
std::wstring widen(std::string_view s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string narrow(std::wstring_view s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
namespace
{
bool same(std::wstring_view a, std::wstring_view b)
{
    return _wcsicmp(std::wstring(a).c_str(), std::wstring(b).c_str()) == 0;
}
std::uint32_t ipv4(const std::string &s)
{
    IN_ADDR a{};
    if (InetPtonA(AF_INET, s.c_str(), &a) != 1)
        throw std::runtime_error("Enter a numeric IPv4 address");
    return ntohl(a.s_addr);
}
std::uint16_t port(const std::wstring &s)
{
    auto text = narrow(s);
    unsigned n{};
    auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (e != std::errc{} || p != text.data() + text.size() || !n || n > 65535)
        throw std::runtime_error("Port must be an integer from 1 to 65535");
    return std::uint16_t(n);
}
std::wstring slot_section(unsigned slot)
{
    if (slot < 1 || slot > 10)
        throw std::runtime_error("Profile number must be 1..10");
    return L"profile." + std::to_wstring(slot);
}
std::wstring slot_key(std::wstring_view section, std::wstring_view key)
{
    if (section == L"alignment")
        return L"alignment_" + std::wstring(key);
    if (section == L"effects")
        return key == L"revision" ? L"effects_revision" : L"parameter_" + std::wstring(key);
    return std::wstring(key);
}
std::wstring setting_text(const ass::graphics::Settings &settings, const wchar_t *section, const wchar_t *key)
{
    uint32_t required{};
    const auto result = ag_settings_get(settings.handle(), section, key, nullptr, 0, &required);
    if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return {};
    ass::graphics::check(result);
    return settings.get(section, key);
}
class ProfileLock
{
    HANDLE handle_{};
    bool owned_{};
  public:
    explicit ProfileLock(const std::filesystem::path &path)
    {
        const auto identity = std::filesystem::absolute(path).lexically_normal().wstring();
        uint64_t hash = 14695981039346656037ull;
        for (auto ch : identity)
        {
            hash ^= uint16_t(towlower(ch));
            hash *= 1099511628211ull;
        }
        const auto name = L"Local\\PSX-IOS.CduProfiles." + std::to_wstring(hash);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_)
            throw std::runtime_error("Profile lock unavailable");
        const auto result = WaitForSingleObject(handle_, 0);
        owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if (!owned_)
        {
            CloseHandle(handle_);
            handle_ = nullptr;
            throw std::runtime_error("Profile file is busy; save again");
        }
        // An abandoned lock still requires a fresh parsed schema before writing.
    }
    ~ProfileLock()
    {
        if (owned_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }
    ProfileLock(const ProfileLock &) = delete;
    ProfileLock &operator=(const ProfileLock &) = delete;
};
} // namespace
std::string validate(const Config &c, const psx::network::Inventory &i, bool require_adapter)
{
    try
    {
        auto host = psx::network::host_ipv4(c.host);auto group = ipv4(c.group);
        if (host.empty())
            return "Host address must be unicast IPv4 (loopback is allowed)";
        if ((group & 0xf0000000u) != 0xe0000000u)
            return "Group must be an IPv4 multicast address";
        if (!c.port)
            return "Ports must be from 1 to 65535";
        if (unsigned(c.unit) > 2 || c.raster > 4 || (c.budget != 256 && c.budget != 512 && c.budget != 1024) ||
            c.placement.size != sizeof(ag_window_placement) || c.placement.version != 1 ||
            c.placement.mode > 2 || c.placement.dpi < 48 || c.placement.dpi > 960 ||
            std::abs(int64_t(c.placement.left)) > 65536 || std::abs(int64_t(c.placement.top)) > 65536 ||
            std::abs(int64_t(c.placement.right)) > 65536 || std::abs(int64_t(c.placement.bottom)) > 65536 ||
            c.placement.right <= c.placement.left || c.placement.bottom <= c.placement.top ||
            std::abs(int64_t(c.display_x)) > 65536 || std::abs(int64_t(c.display_y)) > 65536 || c.display_width < 0 ||
            c.display_width > 16384)
            return "Invalid display settings";
        for (auto &p : parameter_registry())
        {
            auto it = c.parameters.find(p.id);
            float v = it == c.parameters.end() ? p.initial : it->second;
            if (!std::isfinite(v) || v < p.minimum || v > p.maximum || (p.fixed && v != p.initial))
                return "Shader parameter outside audited range: " + std::string(p.id);
        }
        auto value = [&](const char *id) {
            auto i = c.parameters.find(id);
            if (i != c.parameters.end())
                return i->second;
            for (auto &p : parameter_registry())
                if (p.id == std::string_view(id))
                    return p.initial;
            return 0.f;
        };
        if (value("geom_aspect_ratio_x") / value("geom_aspect_ratio_y") > 4.f / 3.f)
            return "Geometry aspect exceeds the pinned mask-scaling maximum of 4:3";
        if (value("beam_min_sigma") > value("beam_max_sigma") || value("beam_min_shape") > value("beam_max_shape"))
            return "Beam minimum must not exceed maximum";
        if(!c.subnet.empty())(void)psx::network::subnet_label(c.subnet);
        if(require_adapter && !host.starts_with("127.") && c.subnet.empty())
            return "Select the CDU network subnet";
        (void)i; // Availability is runtime state; a saved absent subnet remains editable.

    }
    catch (const std::exception &e)
    {
        return e.what();
    }
    return {};
}
psx::network::Adapter resolve(const Config &c, const psx::network::Inventory &i)
{
    psx::network::Adapter result;
    result.guid = c.adapter_guid;
    result.name = L"Saved adapter unavailable";
    for (const auto &a : i.adapters)
        if (same(a.guid, c.adapter_guid))
        {
            result = a;
            break;
        }
    bool ipv4 = false;
    for (const auto &a : result.addresses)
        ipv4 |= a.address.ss_family == AF_INET;
    result.up = result.up && ipv4;
    return result;
}
std::string configured_subnet(const Config&c,const psx::network::Inventory&i)
{
    if(!c.subnet.empty())return c.subnet;
    std::vector<std::string> keys;
    for(const auto&a:i.adapters)if(same(a.guid,c.adapter_guid)){
        auto candidate=psx::network::subnets({{a}});
        keys.insert(keys.end(),candidate.begin(),candidate.end());
    }
    std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
    return keys.size()==1?keys[0]:std::string{};
}
std::wstring select_initial(const psx::network::Inventory &i)
{
    return psx::network::initial_ipv4_adapter(i);
}
Loaded SettingsStore::load() const
{
    Loaded out;
    if (slot_ > 10)
    {
        out.error = "Profile number must be 1..10";
        return out;
    }
    out.missing = !std::filesystem::exists(path_);
    if (out.missing)
        return out;
    try
    {
        ass::graphics::Settings s(path_.c_str());
        s.load();
        const auto section = slot_ ? slot_section(slot_) : std::wstring{};
        if (s.get(L"client", L"schema") != (slot_ ? L"2" : L"1") && s.get(L"client", L"schema") != L"3")
            throw std::runtime_error("Unsupported client profile schema");
        if (slot_)
        {
            const auto present = setting_text(s, section.c_str(), L"present");
            if (present.empty())
            {
                out.missing = true;
                return out;
            }
            if (present != L"1")
                throw std::runtime_error("Invalid profile marker");
        }
        auto get = [&](const wchar_t *category, const wchar_t *key) {
            return slot_ ? setting_text(s, section.c_str(), slot_key(category, key).c_str())
                         : setting_text(s, category, key);
        };
        if (get(L"effects", L"revision") != L"1")
            throw std::runtime_error("Unsupported effects revision");
        auto &c = out.config;
        c.host = narrow(get(L"client", L"host_ipv4"));
        c.group = narrow(get(L"client", L"multicast_group"));
        c.port = port(get(L"client", L"multicast_port"));
        c.adapter_guid = get(L"client", L"adapter_guid");
        c.subnet=narrow(get(L"client",L"subnet_ipv4"));
        if(c.subnet.empty()&&!c.adapter_guid.empty()){
            try{c.subnet=configured_subnet(c,{psx::network::enumerate(psx::network::InventoryPolicy::display)});}catch(...){}
            out.warning=c.subnet.empty()?"Select the CDU subnet; the legacy adapter could not identify one network":"Legacy adapter migrated to CDU subnet; explicit Save profile persists it";
        }
        auto num = [&](const wchar_t *key) {
            auto str = narrow(get(L"client", key));
            unsigned v{};
            auto [p, e] = std::from_chars(str.data(), str.data() + str.size(), v);
            if (e != std::errc{} || p != str.data() + str.size())
                throw std::runtime_error("Malformed numeric profile value");
            return v;
        };
        auto unit = num(L"cdu");
        if (unit > 2)
            throw std::runtime_error("Unsupported CDU selection");
        c.unit = psx::cdu::Unit(unit);
        c.raster = num(L"raster");
        c.budget = num(L"budget_mib");
        auto ef = num(L"effects");
        if (ef > 1)
            throw std::runtime_error("Malformed effects flag");
        c.effects = ef != 0;
        c.placement.mode = num(L"mode");
        auto integer = [&](const wchar_t *key) {
            auto str = narrow(get(L"placement", key));
            int v{};
            auto [p, e] = std::from_chars(str.data(), str.data() + str.size(), v);
            if (e != std::errc{} || p != str.data() + str.size())
                throw std::runtime_error("Malformed placement");
            return v;
        };
        auto optional = [&](const wchar_t *section, const wchar_t *key, int fallback) {
            auto text = narrow(get(section, key));
            if (text.empty())
                return fallback;
            int value{};
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size())
                throw std::runtime_error("Malformed alignment");
            return value;
        };
        c.display_x = optional(L"alignment", L"x", 0);
        c.display_y = optional(L"alignment", L"y", 0);
        c.display_width = optional(L"alignment", L"width", 0);
        c.fullscreen_x = optional(L"placement", L"fullscreen_x", 0);
        c.fullscreen_y = optional(L"placement", L"fullscreen_y", 0);
        const auto anchor = optional(L"placement", L"fullscreen_anchor", 0);
        if (anchor < 0 || anchor > 1)
            throw std::runtime_error("Malformed fullscreen anchor");
        c.fullscreen_anchor = anchor != 0;
        c.placement.left = integer(L"left");
        c.placement.top = integer(L"top");
        c.placement.right = integer(L"right");
        c.placement.bottom = integer(L"bottom");
        c.placement.dpi = integer(L"dpi");
        auto monitor = get(L"placement", L"monitor");
        if (monitor.size() >= 128)
            throw std::runtime_error("Monitor identity too long");
        wcscpy_s(c.placement.monitor, monitor.c_str());
        for (auto &p : parameter_registry())
        {
            auto raw = get(L"effects", widen(p.id).c_str());
            auto str = narrow(raw.empty() ? float_text(p.initial) : raw);
            float v{};
            auto [end, e] = std::from_chars(str.data(), str.data() + str.size(), v);
            if (e != std::errc{} || end != str.data() + str.size())
                throw std::runtime_error("Malformed shader parameter");
            c.parameters[p.id] = v;
        }
        out.error = validate(c, {}, false);
        out.valid = out.error.empty();
    }
    catch (const ass::graphics::Error &e)
    {
        out.error = narrow(e.detail);
    }
    catch (const std::exception &e)
    {
        out.error = e.what();
    }
    if (!out.valid)
        out.config = Config{};
    return out;
}
void SettingsStore::save(const Config &c) const
{
    write(c, slot_);
}
void SettingsStore::save(const Config &c, unsigned slot) const
{
    slot_section(slot);
    write(c, slot);
}
void SettingsStore::write(const Config &c, unsigned slot) const
{
    auto e = validate(c, {});
    if (!e.empty())
        throw std::runtime_error(e);
    const auto section = slot ? slot_section(slot) : std::wstring{};
    ProfileLock lock(path_);
    std::filesystem::create_directories(path_.parent_path());
    ass::graphics::Settings s(path_.c_str());
    if (slot)
    {
        if (std::filesystem::exists(path_))
        {
            s.load();
            if (s.get(L"client", L"schema") != L"2" && s.get(L"client", L"schema") != L"3")
                throw std::runtime_error("Unsupported client profile schema; file retained");
        }
        s.set(L"client", L"schema", L"3");
        s.set(section.c_str(), L"present", L"1");
    }
    else
    {
        try { s.load(); }
        catch (const ass::graphics::Error &) {}
    }
    auto put = [&](const wchar_t *category, const wchar_t *key, const std::wstring &value) {
        if (slot)
            s.set(section.c_str(), slot_key(category, key).c_str(), value.c_str());
        else
            s.set(category, key, value.c_str());
    };
    auto set = [&](const wchar_t *key, const std::wstring &v) { put(L"client", key, v); };
    if (!slot)
        set(L"schema", L"3");
    put(L"effects", L"revision", L"1");
    set(L"host_ipv4", widen(c.host));
    set(L"multicast_group", widen(c.group));
    set(L"multicast_port", std::to_wstring(c.port));
    set(L"adapter_guid", c.adapter_guid);
    set(L"subnet_ipv4", widen(c.subnet));
    set(L"cdu", std::to_wstring(unsigned(c.unit)));
    set(L"raster", std::to_wstring(c.raster));
    set(L"budget_mib", std::to_wstring(c.budget));
    set(L"effects", c.effects ? L"1" : L"0");
    set(L"mode", std::to_wstring(c.placement.mode));
    auto pos = [&](const wchar_t *key, int v) { put(L"placement", key, std::to_wstring(v)); };
    pos(L"left", c.placement.left);
    pos(L"top", c.placement.top);
    pos(L"right", c.placement.right);
    pos(L"bottom", c.placement.bottom);
    pos(L"dpi", c.placement.dpi);
    pos(L"fullscreen_x", c.fullscreen_x);
    pos(L"fullscreen_y", c.fullscreen_y);
    pos(L"fullscreen_anchor", c.fullscreen_anchor ? 1 : 0);
    put(L"alignment", L"x", std::to_wstring(c.display_x));
    put(L"alignment", L"y", std::to_wstring(c.display_y));
    put(L"alignment", L"width", std::to_wstring(c.display_width));
    put(L"placement", L"monitor", c.placement.monitor);
    for (auto &p : parameter_registry())
    {
        auto i = c.parameters.find(p.id);
        float v = i == c.parameters.end() ? p.initial : i->second;
        put(L"effects", widen(p.id).c_str(), float_text(v));
    }
    s.save();
}
unsigned profile_slot(std::string_view s)
{
    if (!s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos)
    {
        if (s == "10")
            return 10;
        if (s.size() == 1 && s[0] >= '1' && s[0] <= '9')
            return unsigned(s[0] - '0');
        throw std::runtime_error("Profile number must be 1..10");
    }
    return 0;
}
std::string profile_name(std::string_view input)
{
    if (input.empty() || input.size() > 32)
        throw std::runtime_error("Profile name must contain 1 to 32 ASCII letters, digits, hyphens or underscores");
    std::string s;
    for (char c : input)
    {
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            throw std::runtime_error("Invalid profile name");
        s += c;
    }
    if (s == "con" || s == "prn" || s == "aux" || s == "nul" || s == "clock$" ||
        (s.size() == 4 && (s.substr(0, 3) == "com" || s.substr(0, 3) == "lpt") && s[3] >= '0' && s[3] <= '9'))
        throw std::runtime_error("Reserved Windows profile name");
    profile_slot(s);
    return s;
}
} // namespace cdu_client
