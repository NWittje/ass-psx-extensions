#include "psx/cdu/render.hpp"
#include <Windows.h>
#include <stdexcept>
namespace psx::cdu
{
namespace
{
void ok(ag_result r)
{
    if (r < 0)
    {
        wchar_t e[1024]{};
        ag_last_error(e, 1024, nullptr);
        int n = WideCharToMultiByte(CP_UTF8, 0, e, -1, nullptr, 0, nullptr, nullptr);
        std::string text(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, e, -1, text.data(), n, nullptr, nullptr);
        throw std::runtime_error(text);
    }
}
ag_display_desc description(unsigned w, unsigned h, bool warp)
{
    return {sizeof(ag_display_desc), 1, uint32_t(warp), AG_RASTER_OUTPUT, screen_width, screen_height, 0, 0, w, h, 0};
}

} // namespace
SurfaceRenderer::SurfaceRenderer(unsigned w, unsigned h, bool warp) : own_context_(true), warp_(warp)
{
    try
    {
        ok(ag_context_create(&context_));
        drawing_ = std::make_unique<Drawing>(context_);
        auto desc = description(w, h, warp);
        ok(ag_display_create(context_, nullptr, {}, &desc, &surface_));
    }
    catch (...)
    {
        ag_destroy(surface_);
        drawing_.reset();
        ag_destroy(context_);
        throw;
    }
}
SurfaceRenderer::SurfaceRenderer(ag_handle context, void *parent, ag_rect bounds, bool warp)
    : context_(context), warp_(warp)
{
    drawing_ = std::make_unique<Drawing>(context_);
    auto desc = description(372, 315, warp);
    ok(ag_display_create(context_, parent, bounds, &desc, &surface_));
}
SurfaceRenderer::~SurfaceRenderer()
{
    ag_destroy(surface_);
    drawing_.reset();
    if (own_context_)
        ag_destroy(context_);
}
void SurfaceRenderer::page(const ScreenView &v)
{
    if (!v.cells)
        throw std::runtime_error("Null screen cells");
    bool same = has_prior_ && prior_ == *v.cells && prior_view_.unit == v.unit &&
                prior_view_.text_valid == v.text_valid && prior_view_.colour_valid == v.colour_valid &&
                prior_view_.appearance_valid == v.appearance_valid && prior_view_.blanking_valid == v.blanking_valid &&
                prior_view_.appearance == v.appearance && prior_view_.blanking == v.blanking;
    if (same)
        return;
    drawing_->page(v);
    ok(ag_display_submit(surface_, drawing_->commands.data(), unsigned(drawing_->commands.size()), nullptr, 0,
                         drawing_->paths.data(), unsigned(drawing_->paths.size())));
    ++content_revision_;
    prior_ = *v.cells;
    prior_view_ = v;
    prior_view_.cells = &prior_;
    has_prior_ = true;
}
void SurfaceRenderer::size(unsigned w, unsigned h)
{
    ok(ag_display_output_size(surface_, w, h));
}
void SurfaceRenderer::fixed_source(unsigned w, unsigned h)
{
    auto desc = description(372, 315, warp_);
    desc.raster_policy = AG_RASTER_FIXED;
    desc.source_width = w;
    desc.source_height = h;
    ok(ag_display_configure(surface_, &desc));
}
void SurfaceRenderer::effects(const ag_effect_graph *g)
{
    ok(ag_display_effects(surface_, g));
}
void SurfaceRenderer::render(double seconds)
{
    ok(ag_display_render(surface_, seconds));
}
void SurfaceRenderer::export_png(const std::filesystem::path &p)
{
    ok(ag_display_export_png(surface_, p.c_str()));
}
void SurfaceRenderer::reset()
{
    ok(ag_device_reset(surface_));
}
ag_display_metrics SurfaceRenderer::metrics() const
{
    ag_display_metrics m{sizeof(m), 1};
    ok(ag_display_metrics_get(surface_, &m));
    return m;
}
} // namespace psx::cdu

namespace psx::cdu
{
void SurfaceRenderer::raster(unsigned w, unsigned h, std::uint64_t budget)
{
    auto d = description(372, 315, warp_);
    d.texture_budget = budget;
    if (w && h)
    {
        d.raster_policy = AG_RASTER_FIXED;
        d.source_width = w;
        d.source_height = h;
    }
    ok(ag_display_configure(surface_, &d));
}
void SurfaceRenderer::parameters(unsigned p, const void *d, unsigned n)
{
    ok(ag_display_parameters(surface_, p, d, n));
}
} // namespace psx::cdu
