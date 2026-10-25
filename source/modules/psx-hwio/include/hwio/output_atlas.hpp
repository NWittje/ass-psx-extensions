#pragma once
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>
namespace hwio {
struct AtlasRectangle {
    double left{},top{},right{},bottom{};
    bool contains(double x,double y)const noexcept;
};
struct OutputAtlasImage {std::string id;std::filesystem::path file;AtlasRectangle world;};
struct OutputAtlasRegion {
    static constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
    std::string id,label,output;
    std::uint32_t parent{none},image{none};
    AtlasRectangle bounds;
};
struct OutputAtlasDeferred {std::string output,reason;};
// Geometry supplies location only. Every assignable leaf is checked against the
// active qualified illumination catalogue before it can be offered to the UI.
class OutputAtlas {
    std::vector<OutputAtlasImage> images_;
    std::vector<OutputAtlasRegion> regions_;
    std::vector<OutputAtlasDeferred> deferred_;
    std::vector<std::uint32_t> children_,offsets_;
    bool complete_{};
public:
    using Qualify=bool(*)(void*,const std::string&);
    bool replace(std::vector<OutputAtlasImage>,std::vector<OutputAtlasRegion>,bool complete,bool require_complete,std::span<const std::string> required_outputs,Qualify,void*,std::string& error,std::vector<OutputAtlasDeferred> deferred={});
    bool load(const std::filesystem::path&,bool require_complete,std::span<const std::string> required_outputs,Qualify,void*,std::string& error);
    std::span<const std::uint32_t> children(std::uint32_t)const noexcept;
    std::uint32_t hit(std::uint32_t,double world_x,double world_y)const noexcept;
    std::vector<std::uint32_t> path(std::uint32_t)const;
    const auto& images()const noexcept{return images_;}
    const auto& regions()const noexcept{return regions_;}
    const auto& deferred()const noexcept{return deferred_;}
    bool complete()const noexcept{return complete_;}
};
}
