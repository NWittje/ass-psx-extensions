#include <du_host/config.hpp>
#include <charconv>
#include <objbase.h>
#include <stdexcept>
#include <fstream>
namespace du_host
{
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
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0,
                                nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), n,
                        nullptr, nullptr);
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
} // namespace
std::string validate(const Config &c, const psx::network::Inventory &i, bool require_adapter)
{
    try
    {
        if(c.du_mask>63||bool(c.du_pid)!=bool(c.du_created))return "Invalid DU source or publication mask";
        if(!c.du_tcp_port||c.du_freshness_ms<250||c.du_freshness_ms>10000)return "Invalid DU TCP port or freshness threshold";
        (void)psx::network::host_ipv4(c.psx);auto group=ipv4(c.group);
        if((group&0xf0000000u)!=0xe0000000u)return "Group must be an IPv4 multicast address";
        if(!c.port||!c.psx_port)return "Ports must be from 1 to 65535";
        (void)i;(void)require_adapter; // Saved GUID is retained only as legacy operator data.

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
std::wstring select_initial(const psx::network::Inventory &i)
{
    return psx::network::initial_ipv4_adapter(i);
}
SettingsStore::SettingsStore(std::filesystem::path path,std::filesystem::path legacy) : path_(std::move(path)),legacy_(std::move(legacy))
{
    if (path_.empty())
    {
        std::wstring exe(32768, 0);
        auto n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
        if (!n || n >= exe.size())
            throw std::runtime_error("Cannot locate settings directory");
        exe.resize(n);
        path_ = std::filesystem::path(exe).parent_path() / L"settings.cfg";
        if(path_.parent_path().filename()==L"psx-du-host")legacy_=path_.parent_path().parent_path()/L"psx-cdu-host"/L"settings.cfg";
    }
}
Loaded SettingsStore::load() const
{
    Loaded out;
    auto selected=path_;
    const bool old=!legacy_.empty()&&std::filesystem::is_regular_file(legacy_);
    if(!std::filesystem::exists(path_)&&old){selected=legacy_;out.warning="Loaded legacy host settings; explicit Apply saves at the new location. Original settings and backup are preserved.";}
    else if(old){out.warning="New-location settings take precedence. Preserved legacy settings also exist; review them before discarding either copy.";}
    out.missing = !std::filesystem::exists(selected);
    if (out.missing)
    {
        out.valid = true;
        return out;
    }
    try
    {
        ass::graphics::Settings s(selected.c_str());
        s.load();
        if (s.get(L"host", L"schema") != L"1" && s.get(L"host",L"schema")!=L"2" && s.get(L"host",L"schema")!=L"3" && s.get(L"host",L"schema")!=L"4")
            throw std::runtime_error("Unsupported host settings schema");
        out.config.psx = narrow(s.get(L"host", L"psx_ipv4"));
        out.config.group = narrow(s.get(L"host", L"multicast_group"));
        out.config.psx_port = port(s.get(L"host", L"psx_port"));
        out.config.port = port(s.get(L"host", L"multicast_port"));
        out.config.adapter_guid = s.get(L"host", L"adapter_guid");
        if(s.get(L"host",L"schema")!=L"1"){
            auto number=[&](const wchar_t* key){auto text=s.get(L"host",key);size_t used{};auto value=std::stoull(text,&used);if(used!=text.size()||text.starts_with(L"-"))throw std::runtime_error("Invalid DU configuration number");return value;};
            auto mask=number(L"du_mask"),pid=number(L"du_pid");if(mask>63||pid>UINT32_MAX)throw std::runtime_error("Invalid DU configuration range");out.config.du_mask=unsigned(mask);out.config.du_pid=uint32_t(pid);out.config.du_created=number(L"du_created");
            if((s.get(L"host",L"schema")==L"3" || s.get(L"host",L"schema")==L"4")){
                auto tcp=number(L"du_tcp_port"),fresh=number(L"du_freshness_ms");
                if(!tcp||tcp>65535||fresh<250||fresh>10000)throw std::runtime_error("Invalid DU TCP configuration");
                out.config.du_tcp_port=uint16_t(tcp);out.config.du_freshness_ms=unsigned(fresh);
            }
        }
        out.error = validate(out.config, {}, false);
        out.valid = out.error.empty();
        if(out.valid&&s.get(L"host",L"schema")!=L"4")
            out.warning+=" IPv4 publication now uses all eligible adapters; explicit Apply saves schema 4.";

    }
    catch (const ass::graphics::Error &e)
    {
        out.error = narrow(e.detail);
    }
    catch (const std::exception &e)
    {
        out.error = e.what();
    }
    return out;
}
void SettingsStore::save(const Config &c) const
{
    auto error = validate(c, {});
    if (!error.empty())
        throw std::runtime_error(error);
    ass::graphics::Settings s(path_.c_str());
    try
    {
        s.load();
    }
    catch (const ass::graphics::Error &)
    { /* Explicit Apply repairs; save keeps backup. */
    }
    s.set(L"host", L"schema", L"4");
    s.set(L"host", L"psx_ipv4", widen(c.psx).c_str());
    s.set(L"host", L"psx_port", std::to_wstring(c.psx_port).c_str());
    s.set(L"host", L"multicast_group", widen(c.group).c_str());
    s.set(L"host", L"multicast_port", std::to_wstring(c.port).c_str());
    s.set(L"host", L"adapter_guid", c.adapter_guid.c_str());
    s.set(L"host",L"du_mask",std::to_wstring(c.du_mask).c_str());s.set(L"host",L"du_pid",std::to_wstring(c.du_pid).c_str());s.set(L"host",L"du_created",std::to_wstring(c.du_created).c_str());
    s.set(L"host",L"du_tcp_port",std::to_wstring(c.du_tcp_port).c_str());
    s.set(L"host",L"du_freshness_ms",std::to_wstring(c.du_freshness_ms).c_str());
    s.save();
}
} // namespace du_host
