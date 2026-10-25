#pragma once
#include "config.hpp"
#include <span>
#include <array>
#include <vector>
namespace cdu_client
{
struct Parameter
{
    const char *id;
    const wchar_t *label, *group;
    float initial, minimum, maximum, step;
    bool fixed;
};
std::span<const Parameter> parameter_registry();
// Immutable, hash-verified runtime assets; no preset parsing or shader compiler.
// A client retains at most one cache, bounded to32MiB, for effect transactions.
class ShaderAssets
{
    std::array<std::vector<uint8_t>, 30> bytes_;
  public:
    explicit ShaderAssets(const std::filesystem::path &);
    ShaderAssets(const ShaderAssets &) = delete;
    ShaderAssets &operator=(const ShaderAssets &) = delete;
    const std::vector<uint8_t> &get(std::string_view) const;
};
struct Candidate
{
    ag_handle value{};
    Candidate() = default;
    Candidate(const Candidate &) = delete;
    Candidate &operator=(const Candidate &) = delete;
    Candidate(Candidate &&v) noexcept : value(std::exchange(v.value, nullptr))
    {
    }
    ~Candidate()
    {
        if (value)
            ag_display_abort(value);
    }
    void commit(ag_handle surface)
    {
        ass::graphics::check(ag_display_commit(surface, value));
    }
};
void validate_viewport(ag_handle surface, const Config &);
Candidate prepare_display(ag_handle surface, const Config &, const std::filesystem::path &, bool warp = false,
                          const ShaderAssets *cache = nullptr);
void update_parameters(ag_handle surface, const Config &);
std::filesystem::path executable_directory();
} // namespace cdu_client
