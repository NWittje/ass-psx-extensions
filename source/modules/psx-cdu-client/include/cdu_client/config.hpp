#pragma once
#include <ass_graphics/display.hpp>
#include <filesystem>
#include <map>
#include <psx/cdu/transport.hpp>
namespace cdu_client
{
std::wstring widen(std::string_view);
std::string narrow(std::wstring_view);
std::string profile_name(std::string_view);
// Zero denotes a legacy named profile; numbered slots are exactly 1 through 10.
unsigned profile_slot(std::string_view);
struct Config
{
    std::string host, group{"239.255.74.74"};
    uint16_t port{47474};
    std::wstring adapter_guid; // Preserved legacy data; never the new binding policy.
    std::string subnet;
    psx::cdu::Unit unit{};
    unsigned raster{}, budget{256};
    bool effects{};
    // Alignment is measured in physical pixels and applies only in fullscreen.
    int display_x{}, display_y{}, display_width{};
    int fullscreen_x{}, fullscreen_y{};
    bool fullscreen_anchor{};
    ag_window_placement placement{sizeof(ag_window_placement), 1, {}, 100, 100, 844, 770, 96, 0};
    std::map<std::string, float> parameters;
};
std::string validate(const Config &, const psx::network::Inventory &, bool require_adapter = true);
psx::network::Adapter resolve(const Config &, const psx::network::Inventory &);
std::string configured_subnet(const Config&,const psx::network::Inventory&);
std::wstring select_initial(const psx::network::Inventory &);
struct Loaded
{
    Config config;
    bool missing{}, valid{};
    std::string error,warning;
};
class SettingsStore
{
    std::filesystem::path path_;
    unsigned slot_{};
    void write(const Config &, unsigned slot) const;

  public:
    explicit SettingsStore(std::filesystem::path p, unsigned slot = 0) : path_(std::move(p)), slot_(slot)
    {
    }
    Loaded load() const;
    void save(const Config &) const;
    void save(const Config &, unsigned slot) const;
    const auto &path() const
    {
        return path_;
    }
};
} // namespace cdu_client
