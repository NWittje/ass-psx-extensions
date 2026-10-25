#pragma once
#include <psx/display/transport.hpp>
#include <ass_graphics/display.hpp>
#include <filesystem>
#include <string>
#include <string_view>
namespace du_client
{
struct TrimAlignment
{
    bool enabled{};
    bool always_show{};
    int x{}, y{}, width{};
};
struct Config
{
    ag_window_placement placement{sizeof(ag_window_placement), 1, {}, 100, 100, 900, 700, 96, 0};
    int x{}, y{}, width{}, fullscreen_x{}, fullscreen_y{};
    bool anchor{};
    unsigned unit{};
    bool network{true};
    std::string host{"127.0.0.1"};
    std::wstring adapter_guid;
    uint16_t tcp_port{47480};
    TrimAlignment trim;
};
std::wstring profile_name(std::wstring_view);
std::wstring user_identity();
std::filesystem::path executable_directory();
void validate(const Config &);
// Validate a computer name or IPv4 target; DNS resolution stays on the reader.
std::string network_host(std::string_view);
struct Loaded
{
    Config config;
    bool missing{}, valid{};
    std::wstring error;
    std::wstring warning;
};
class SettingsStore
{
    std::filesystem::path path_;
    unsigned profile_;

  public:
    explicit SettingsStore(std::filesystem::path p, unsigned profile = 1)
        : path_(std::move(p)), profile_(profile)
    {
    }
    Loaded load() const;
    void save(const Config &) const;
    void save(const Config &, unsigned profile) const;
    const auto &path() const
    {
        return path_;
    }
};
} // namespace du_client
