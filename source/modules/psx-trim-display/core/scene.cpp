#include "trim/scene.hpp"
#include <algorithm>

namespace trim {
static bool touches(Rect a,Rect b) noexcept {return a.x<=b.x+b.w&&b.x<=a.x+a.w&&a.y<=b.y+b.h&&b.y<=a.y+a.h;}
static Rect unite(Rect a,Rect b) noexcept {const int x=std::min(a.x,b.x),y=std::min(a.y,b.y);return {x,y,std::max(a.x+a.w,b.x+b.w)-x,std::max(a.y+a.h,b.y+b.h)-y};}
void DirtyRegions::add(Rect r) noexcept {
    if(r.w<=0||r.h<=0)return;
    // Clip wide endpoints before narrowing to panel pixels. Malformed geometry
    // must not wrap into a valid DMA region.
    const int right=int(std::clamp<std::int64_t>(std::int64_t(r.x)+r.w,0,width));
    const int bottom=int(std::clamp<std::int64_t>(std::int64_t(r.y)+r.h,0,height));
    r.x=std::clamp(r.x,0,width);r.y=std::clamp(r.y,0,height);
    // RGB565 DMA rows contain an even number of pixels; repaint expanded background too.
    r.x &= ~1;r.w=std::min(width,(right+1)&~1)-r.x;r.h=bottom-r.y;
    if(r.w<=0||r.h<=0)return;
    for(std::size_t i=0;i<count;){if(touches(r,rects[i])){r=unite(r,rects[i]);rects[i]=rects[--count];i=0;}else ++i;}
    if(count==rects.size()){count=1;rects[0]=screen;return;}rects[count++]=r;
}
DirtyRegions TrimScene::changes(const Visual& n,bool full) const noexcept {
    DirtyRegions d;
    if(full||!has_last_){d.add(screen);return d;}
    if(n.mode!=last_.mode){d.add(viewport);d.add(link_badge);return d;}
    if(n.mode==Mode::normal){
        if(n.bar_height!=last_.bar_height){const auto lo=std::min(n.bar_height,last_.bar_height);const auto hi=std::max(n.bar_height,last_.bar_height);d.add({bar.x,bar.y+lo,bar.w,hi-lo});}
        if((n.green^last_.green)&1)d.add(green_down);
        if((n.green^last_.green)&2)d.add(green_up);
    }
    return d;
}
static bool badge_pixel(int x,int y) noexcept {
    // Deliberately distinct from aircraft OFF. Tiny native-independent LINK annotation in margin.
    constexpr std::array<std::array<std::uint8_t,7>,4> glyph{{{{16,16,16,16,16,16,31}},{{31,4,4,4,4,4,31}},{{17,25,25,21,19,19,17}},{{17,18,20,24,20,18,17}}}};
    const int px=(x-link_badge.x-4)/2,py=(y-link_badge.y-4)/2;
    if(x<link_badge.x+4||y<link_badge.y+4||px<0||py<0||py>=7||px>=24)return false;
    const int g=px/6,col=px%6;return g<4&&col<5&&(glyph[std::size_t(g)][std::size_t(py)]&(1u<<(4-col)));
}
bool TrimScene::compose(Rect r,const Visual& v,std::span<std::uint16_t> out) const noexcept {
    if(r.x<0||r.y<0||r.x>=width||r.y>=height||r.w<=0||r.h<=0||
       r.w>width-r.x||r.h>height-r.y||std::size_t(r.w)*std::size_t(r.h)>out.size())return false;
    for(int dy=0;dy<r.h;++dy)for(int dx=0;dx<r.w;++dx){
        const int x=r.x+dx,y=r.y+dy;auto color=assets::background(role_,x,y);
        if(v.mode==Mode::normal){
            if(bar.contains(x,y)&&y<bar.y+v.bar_height)color=assets::bar_pixel(role_,x,y);
            if((v.green&1)&&green_down.contains(x,y))color=assets::green_pixel(role_,false,x,y);
            if((v.green&2)&&green_up.contains(x,y))color=assets::green_pixel(role_,true,x,y);
        }else if(v.mode==Mode::off && off_plate.contains(x,y))color=assets::off_pixel(role_,x,y);
        if(v.mode==Mode::unavailable && badge_pixel(x,y))color=assets::unavailable_color;
        out[std::size_t(dy*r.w+dx)]=color;
    }
    return true;
}
int DmaSlots::claim() noexcept {for(unsigned i=0;i<2;++i)if(states_[i]==0){states_[i]=1;return int(i);}return -1;}
bool DmaSlots::submit(int slot) noexcept {if(slot<0||slot>=2||states_[std::size_t(slot)]!=1||pending_==2)return false;states_[std::size_t(slot)]=2;fifo_[(head_+pending_)%2]=std::uint8_t(slot);++pending_;return true;}
bool DmaSlots::abandon(int slot) noexcept {if(slot<0||slot>=2||states_[std::size_t(slot)]!=1)return false;states_[std::size_t(slot)]=0;return true;}
bool DmaSlots::complete() noexcept {if(!pending_)return false;states_[fifo_[head_]]=0;head_=(head_+1)%2;--pending_;return true;}
}
