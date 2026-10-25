#include "psx/cdu/render.hpp"
#include <cmath>
#include <stdexcept>
namespace psx::cdu
{
namespace
{
void check(ag_result r)
{
    if (r < 0)
    {
        wchar_t e[1024]{};
        ag_last_error(e, 1024, nullptr);
        throw std::runtime_error("ASS Graphics operation failed: HRESULT " + std::to_string(r));
    }
}
ag_color colour(char c)
{
    switch (c)
    {
    case 'g':
        return {0, 1, 0, 1};
    case 'm':
        return {1, 0, 1, 1};
    case 'c':
        return {0, 1, 1, 1};
    case 'a':
        return {1, .65f, 0, 1};
    case 'r':
        return {1, 0, 0, 1};
    case 'b':
        return {0, .35f, 1, 1};
    default:
        return {1, 1, 1, 1};
    }
}
ag_draw transform(float s, float x, float y)
{
    ag_draw d{};
    d.kind = AG_TRANSFORM;
    d.matrix[0] = d.matrix[3] = s;
    d.matrix[4] = x;
    d.matrix[5] = y;
    return d;
}
unsigned index(char c, Size s)
{
    return static_cast<unsigned char>(c) * 2 + (s == Size::large);
}
void glyph(std::vector<ag_draw> &draws, std::vector<ag_path_draw> &paths, ag_handle handle,
           ag_color col, float scale, float x, float y)
{
    if (!handle)
        return;
    draws.push_back(transform(scale, x, y));
    ag_draw d{};
    d.kind = AG_PATH;
    d.color = col;
    d.width = 2;
    d.first_point = unsigned(paths.size());
    paths.push_back({handle, 0, AG_CAP_ROUND, AG_JOIN_ROUND});
    draws.push_back(d);
}
} // namespace
Drawing::Drawing(ag_handle context):context_(context) {
    try {
        commands.reserve(2016);paths.reserve(1008);
        for(auto &g:glyphs())if(!g.contours.empty())
            check(ag_path_create(context_,g.contours.data(),unsigned(g.contours.size()),&paths_[index(g.code,g.size)]));
        const ag_path_command rectangle[]={{AG_PATH_MOVE,{-7,-9}},{AG_PATH_LINE,{7,-9}},{AG_PATH_LINE,{7,9}},{AG_PATH_LINE,{-7,9}},{AG_PATH_CLOSE}};
        check(ag_path_create(context_,rectangle,5,&highlight_));
    }catch(...){ag_destroy(highlight_);for(auto p:paths_)ag_destroy(p);throw;}
}
Drawing::~Drawing(){ag_destroy(highlight_);for(auto p:paths_)ag_destroy(p);}
Renderer::Renderer(){try{
    check(ag_context_create(&context_));ag_theme theme{};check(ag_theme_get(context_,&theme));theme.background={0,0,0,1};check(ag_theme_set(context_,&theme));
    drawing_=std::make_unique<Drawing>(context_);
    check(ag_window_create(context_,L"CDU qualification (hidden)",900,400,nullptr,nullptr,&window_));
    void *hwnd{};check(ag_native_window(window_,&hwnd));check(ag_canvas_create(context_,hwnd,{0,0,900,400},&canvas_));
}catch(...){ag_destroy(canvas_);ag_destroy(window_);drawing_.reset();ag_destroy(context_);throw;}}
Renderer::~Renderer(){ag_destroy(canvas_);ag_destroy(window_);drawing_.reset();ag_destroy(context_);}
void Renderer::reset(){check(ag_device_reset(canvas_));}
void Renderer::page(const CduScreen &screen, const std::filesystem::path &file, float scale)
{
    page(screen_view(screen), file, scale);
}
void Drawing::page(const ScreenView &screen,float scale)
{
    if (!std::isfinite(scale) || scale <= 0 || scale > 8)
        throw std::runtime_error("Invalid uniform scale");
    if(!screen.cells || unsigned(screen.unit)>2)throw std::runtime_error("Invalid CDU screen view");
    commands.clear();paths.clear();auto &d=commands;auto &p=paths;
    if (screen.blanking_valid && screen.blanking == 0 && screen.appearance_valid)
    {
        for (unsigned row = 0; row < rows; ++row)
        {
            if (!(screen.text_valid & (1u << row)))
                continue;
            for (unsigned col = 0; col < columns; ++col)
            {
                auto &cell = (*screen.cells)[row][col];
                float x = origin_x + column_advance * col, y = origin_y + row_advance * row;
                if (screen.lcd() && cell.highlighted() && (screen.colour_valid & (1u << row)))
                {
                    d.push_back(transform(scale, x * scale, y * scale));
                    ag_draw h{};
                    h.kind = AG_PATH;
                    h.color = {.35f, .35f, .35f, 1};
                    h.width = 2;
                    h.first_point = unsigned(p.size());
                    p.push_back({highlight_, 0, AG_CAP_ROUND, AG_JOIN_ROUND});
                    d.push_back(h);
                    h.first_point = unsigned(p.size());
                    p.push_back({highlight_, 1, AG_CAP_ROUND, AG_JOIN_ROUND});
                    d.push_back(h);
                }
                if (cell.blank())
                    continue;
                glyph(d, p, paths_[index(cell.raw, cell.size)],
                      screen.lcd() ? colour(cell.colour) : ag_color{0, 1, 0, 1}, scale, x * scale,
                      y * scale);
            }
        }
    }
}
void Renderer::page(const ScreenView &screen,const std::filesystem::path &file,float scale){
    drawing_->page(screen,scale);auto &d=drawing_->commands;auto &p=drawing_->paths;
    check(ag_canvas_submit_paths(canvas_,d.data(),unsigned(d.size()),nullptr,0,p.data(),unsigned(p.size())));
    check(ag_canvas_export_png(canvas_,file.c_str(),unsigned(std::lround(screen_width*scale)),unsigned(std::lround(screen_height*scale)),96));
}
void Drawing::sheet()
{
    commands.clear();paths.clear();auto &d=commands;auto &p=paths;
    unsigned i = 0;
    for (char c : glyph_codes)
    {
        float x = 25 + (i % 18) * 49.f, y = 40 + (i / 18) * 90.f;
        ag_draw label{};
        label.kind = AG_TEXT;
        label.rect = {x - 5, y - 32, x + 30, y - 13};
        label.color = {.5f, .5f, .5f, 1};
        label.font_size = 10;
        label.text[0] = c;
        d.push_back(transform(1, 0, 0));
        d.push_back(label);
        for (Size s : {Size::small, Size::large})
            glyph(d, p, paths_[index(c, s)], {1, 1, 1, 1}, 1, x, y + (s == Size::large ? 32 : 0));
        ++i;
    }
 }
void Renderer::sheet(const std::filesystem::path &file){drawing_->sheet();auto &d=drawing_->commands;auto &p=drawing_->paths;check(ag_canvas_submit_paths(canvas_,d.data(),unsigned(d.size()),nullptr,0,p.data(),unsigned(p.size())));check(ag_canvas_export_png(canvas_,file.c_str(),900,400,96));}
} // namespace psx::cdu
