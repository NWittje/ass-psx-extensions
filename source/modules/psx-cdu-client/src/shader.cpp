#include <cdu_client/shader.hpp>

#include <algorithm>
#include <bcrypt.h>
#include <cmath>
#include <cstring>
#include <fstream>
namespace cdu_client
{
namespace
{
struct InputDefinition
{
    unsigned slot, kind, index, linear, repeat, mips, srgb;
};
struct UniformValue
{
    unsigned slot, offset;
    const char *parameter;
    float literal;
};
struct DynamicValue
{
    unsigned slot, offset, kind, index;
};
struct BufferDefinition
{
    unsigned slot, bytes;
};
struct LutDefinition
{
    const char *name;
    unsigned width, height;
    bool mips;
    const char *file;
};
struct PassDefinition
{
    const char *name;
    unsigned xkind, ykind;
    float x, y;
    bool srgb, mips;
    std::span<const InputDefinition> inputs;
    std::span<const UniformValue> values;
    std::span<const DynamicValue> dynamic;
    std::span<const BufferDefinition> buffers;
};
struct Asset
{
    const char *file, *hash;
};
#include "registry.hpp"
std::vector<uint8_t> load(const std::filesystem::path &root, const std::string &name)
{
    auto a = std::find_if(std::begin(assets), std::end(assets), [&](auto &v) { return v.file == name; });
    if (a == std::end(assets))
        throw std::runtime_error("Shader asset absent from registry: " + name);
    std::ifstream f(root / name, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Missing shader asset: " + name);
    auto len = f.tellg();
    if (len < 0 || len > 16 * 1024 * 1024)
        throw std::runtime_error("Invalid shader asset size: " + name);
    std::vector<uint8_t> bytes(size_t(len), 0);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(bytes.data()), len))
        throw std::runtime_error("Cannot read shader asset: " + name);
    uint8_t hash[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), ULONG(bytes.size()), hash, 32) < 0)
        throw std::runtime_error("Shader hash unavailable");
    std::string hex;
    const char *digits = "0123456789abcdef";
    for (auto c : hash)
    {
        hex += digits[c >> 4];
        hex += digits[c & 15];
    }
    if (hex != a->hash)
        throw std::runtime_error("Shader asset hash mismatch: " + name);
    return bytes;
}
std::vector<uint8_t> constants(const PassDefinition &p, const BufferDefinition &b, const Config &c)
{
    std::vector<uint8_t> data((b.bytes + 15) & ~15u);
    for (auto &v : p.values)
        if (v.slot == b.slot)
        {
            float n = v.literal;
            if (v.parameter[0])
            {
                auto i = c.parameters.find(v.parameter);
                if (i != c.parameters.end())
                    n = i->second;
                else
                {
                    auto p = std::find_if(std::begin(parameters), std::end(parameters),
                                          [&](auto &p) { return std::string_view(p.id) == v.parameter; });
                    if (p == std::end(parameters))
                        throw std::runtime_error("Invalid parameter registry");
                    n = p->initial;
                }
            }
            memcpy(data.data() + v.offset, &n, 4);
        }
    return data;
}
} // namespace
std::span<const Parameter> parameter_registry()
{
    return parameters;
}
ShaderAssets::ShaderAssets(const std::filesystem::path &root)
{
    static_assert(std::size(assets) == 30);
    size_t total{};
    for (size_t i = 0; i < std::size(assets); ++i)
    {
        bytes_[i] = load(root, assets[i].file);
        if (bytes_[i].size() > 32u * 1024 * 1024 - total)
            throw std::runtime_error("Shader asset cache exceeds32MiB");
        total += bytes_[i].size();
    }
}
const std::vector<uint8_t> &ShaderAssets::get(std::string_view name) const
{
    for (size_t i = 0; i < std::size(assets); ++i)
        if (name == assets[i].file)
            return bytes_[i];
    throw std::runtime_error("Shader asset absent from cache: " + std::string(name));
}
std::filesystem::path executable_directory()
{
    std::wstring s(32768, 0);
    auto n = GetModuleFileNameW(nullptr, s.data(), DWORD(s.size()));
    if (!n || n >= s.size())
        throw std::runtime_error("Executable directory unavailable");
    s.resize(n);
    return std::filesystem::path(s).parent_path();
}
void validate_viewport(ag_handle surface, const Config &c)
{
    ag_display_metrics metrics{sizeof(metrics), 1};
    ass::graphics::check(ag_display_metrics_get(surface, &metrics));
    void *native{};
    // Offscreen displays deliberately have no HWND; metrics supply their viewport.
    ag_native_window(surface, &native);
    unsigned vw = metrics.viewport_width, vh = metrics.viewport_height;
    if (native)
    {
        RECT r{};
        GetClientRect(static_cast<HWND>(native), &r);
        double scale = std::min(double(r.right) / 372, double(r.bottom) / 315);
        vw = unsigned(std::lround(372 * scale));
        vh = unsigned(std::lround(315 * scale));
    }
    auto mode = c.parameters.find("mask_sample_mode_desired");
    bool manual = mode == c.parameters.end() || mode->second < .5f;
    // The pinned flat-twice path needs two 16-texel tiles (8 triads, minimum
    // 2 texels/triad) in the 1/16-viewport resize targets. Hardware mask modes
    // do not consume MASK_RESIZE, but all pass targets must remain nonzero.
    if (vw && (vw < (manual ? 512u : 16u) || vh < (manual ? 512u : 16u)))
        throw std::runtime_error(manual ? "Manual CRT-Royale mask needs a fitted viewport of at least 512 x 512 "
                                          "pixels; choose a hardware mask mode or enlarge the window"
                                        : "CRT-Royale needs a fitted viewport of at least 16 x 16 pixels");
}
Candidate prepare_display(ag_handle surface, const Config &c, const std::filesystem::path &assets_root, bool warp,
                          const ShaderAssets *cache)
{
    auto e = validate(c, {}, false);
    if (!e.empty())
        throw std::runtime_error(e);
    ag_display_desc description{
        sizeof(ag_display_desc),         1, uint32_t(warp), uint32_t(AG_RASTER_OUTPUT), 372, 315, 0, 0, 372, 315,
        uint64_t(c.budget) * 1024 * 1024};
    Candidate candidate;
    if (!c.effects)
    {
        ass::graphics::check(ag_display_prepare(surface, &description, nullptr, &candidate.value));
        return candidate;
    }
    validate_viewport(surface, c);
    std::array<ag_effect_pass, 12> passes{};
    std::array<std::vector<ag_effect_input>, 12> inputs;
    std::array<std::vector<uint8_t>, 24> bytecode;
    std::array<ag_lookup_texture, 6> lookups{};
    std::array<std::vector<uint8_t>, 6> pixels;
    for (unsigned i = 0; i < 6; ++i)
    {
        auto &l = luts[i];
        if (!cache)
            pixels[i] = load(assets_root, l.file);
        const auto &data = cache ? cache->get(l.file) : pixels[i];
        auto &t = lookups[i];
        t = {sizeof(t), 1};
        strcpy_s(t.name, l.name);
        t.width = l.width;
        t.height = l.height;
        t.format = AG_RGBA8;
        t.stride = l.width * 4;
        t.mipmaps = l.mips;
        t.pixels = data.data();
        t.bytes = unsigned(data.size());
    }
    for (unsigned i = 0; i < 12; ++i)
    {
        auto &p = stages[i];
        auto &d = passes[i];
        d = {sizeof(d), 1};
        strcpy_s(d.name, p.name);
        const auto vertex_name = std::to_string(i) + ".vert.cso", pixel_name = std::to_string(i) + ".frag.cso";
        if (!cache)
        {
            bytecode[i * 2] = load(assets_root, vertex_name);
            bytecode[i * 2 + 1] = load(assets_root, pixel_name);
        }
        const auto &vertex = cache ? cache->get(vertex_name) : bytecode[i * 2];
        const auto &pixel = cache ? cache->get(pixel_name) : bytecode[i * 2 + 1];
        d.vertex_bytecode = vertex.data();
        d.vertex_bytes = unsigned(vertex.size());
        d.pixel_bytecode = pixel.data();
        d.pixel_bytes = unsigned(pixel.size());
        for (auto &v : p.inputs)
        {
            ag_effect_input input{sizeof(input), 1, v.kind, v.index, v.slot, v.linear, v.repeat, v.mips, v.srgb};
            inputs[i].push_back(input);
        }
        d.inputs = inputs[i].data();
        d.input_count = unsigned(inputs[i].size());
        d.scale_x_kind = p.xkind;
        d.scale_y_kind = p.ykind;
        d.scale_input = 0;
        d.scale_x = p.x;
        d.scale_y = p.y;
        d.format = AG_RGBA8;
        d.srgb = p.srgb;
        d.mipmaps = p.mips;
    }
    ag_effect_graph graph{sizeof(graph), 1, passes.data(), 12, lookups.data(), 6, 0, 0};
    ass::graphics::check(ag_display_prepare(surface, &description, &graph, &candidate.value));
    for (unsigned i = 0; i < 12; ++i)
        for (auto &buffer : stages[i].buffers)
        {
            auto data = constants(stages[i], buffer, c);
            std::vector<ag_constant_binding> bindings;
            for (auto &v : stages[i].dynamic)
                if (v.slot == buffer.slot)
                    bindings.push_back({sizeof(ag_constant_binding), 1, v.offset, v.kind, v.index});
            ass::graphics::check(ag_display_candidate_constants(candidate.value, i, buffer.slot, data.data(),
                                                                unsigned(data.size()), bindings.data(),
                                                                unsigned(bindings.size())));
        }
    return candidate;
}
void update_parameters(ag_handle surface, const Config &c)
{
    for (unsigned i = 0; i < 12; ++i)
        for (auto &b : stages[i].buffers)
            if (b.slot == 1)
            {
                auto data = constants(stages[i], b, c);
                ass::graphics::check(ag_display_constants(surface, i, b.slot, data.data(), unsigned(data.size())));
            }
}
} // namespace cdu_client
