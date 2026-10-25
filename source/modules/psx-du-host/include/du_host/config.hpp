#pragma once
#include <ass_graphics/graphics.hpp>
#include <filesystem>
#include <psx/cdu/transport.hpp>
namespace du_host
{
struct Config
{
    std::string psx{"127.0.0.1"}, group{"239.255.74.74"};
    std::uint16_t psx_port{10747}, port{47474};
    std::wstring adapter_guid;
    unsigned du_mask{63};
    uint32_t du_pid{};uint64_t du_created{};
    std::uint16_t du_tcp_port{47480};
    unsigned du_freshness_ms{2000};
    bool operator==(const Config &) const = default;
};
std::wstring widen(std::string_view);
std::string narrow(std::wstring_view);
// Local sources/publication do not require a physical interface. The stricter
// check is used only for LAN diagnostics, never as a local-service gate.
std::string validate(const Config &, const psx::network::Inventory &, bool require_adapter = false);
psx::network::Adapter resolve(const Config &, const psx::network::Inventory &);
std::wstring select_initial(const psx::network::Inventory &);
struct Loaded
{
    Config config;
    bool missing{}, valid{};
    std::string error;
    std::string warning;
};
class SettingsStore
{
    std::filesystem::path path_;
    std::filesystem::path legacy_;

  public:
    explicit SettingsStore(std::filesystem::path path = {},std::filesystem::path legacy = {});
    Loaded load() const;
    // Explicit user Apply may repair malformed settings; prior bytes become .bak.
    void save(const Config &) const;
    const std::filesystem::path &path() const noexcept
    {
        return path_;
    }
};
} // namespace du_host
