#pragma once
#include "glyphs.hpp"
#include <filesystem>
#include <memory>
namespace psx::cdu
{
// Shared command builder; geometry is registered once on the owning UI thread.
class Drawing {
    ag_handle context_{}, highlight_{};
    std::array<ag_handle,512> paths_{};
public:
    std::vector<ag_draw> commands;
    std::vector<ag_path_draw> paths;
    explicit Drawing(ag_handle);
    ~Drawing();
    Drawing(const Drawing&)=delete;
    Drawing& operator=(const Drawing&)=delete;
    void page(const ScreenView&,float scale=1);
    void sheet();
};
// UI-thread renderer; glyph handles constructed once and reused. No font prep
// or vector text parsing on updates. Brightness is deliberately not normalized.
class Renderer
{
    ag_handle context_{}, window_{}, canvas_{};
    std::unique_ptr<Drawing> drawing_;

  public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void page(const CduScreen &, const std::filesystem::path &, float scale = 1);
    void page(const ScreenView &, const std::filesystem::path &, float scale = 1);
    void sheet(const std::filesystem::path &);
    void reset();
};
// Production surface adapter: no PSX decoding or networking. With a supplied
// context, caller keeps it and parent alive until this adapter is destroyed.
class SurfaceRenderer {
    ag_handle context_{}, surface_{}; bool own_context_{}, warp_{};
    std::unique_ptr<Drawing> drawing_;
    Cells prior_{}; ScreenView prior_view_{}; bool has_prior_{};
    std::uint64_t content_revision_{};
public:
    SurfaceRenderer(unsigned width=372,unsigned height=315,bool warp=false);
    SurfaceRenderer(ag_handle context,void *parent,ag_rect bounds,bool warp=false);
    ~SurfaceRenderer();
    SurfaceRenderer(const SurfaceRenderer&)=delete;
    SurfaceRenderer& operator=(const SurfaceRenderer&)=delete;
    void page(const ScreenView&);
    // Submission identity, not GPU/display completion; unchanged pages retain it.
    std::uint64_t content_revision() const noexcept { return content_revision_; }
    void size(unsigned width,unsigned height);
    void raster(unsigned width=0,unsigned height=0,std::uint64_t budget=0);
    void parameters(unsigned pass,const void *data,unsigned bytes);
    void fixed_source(unsigned width,unsigned height);
    void effects(const ag_effect_graph*);
    void render(double seconds);
    void export_png(const std::filesystem::path&);
    void reset();
    ag_display_metrics metrics() const;
    ag_handle handle() const {return surface_;}
};
void qualify_surface(const std::filesystem::path&,const std::filesystem::path&);
} // namespace psx::cdu
