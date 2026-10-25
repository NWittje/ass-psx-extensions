#pragma once
#include <ass_graphics/replay.h>
#include <memory>
#include <psx/display/dpf_protocol.hpp>
#include <span>
#include <vector>
namespace psx::du
{
struct Resource
{
    ag_replay_resource descriptor{sizeof(ag_replay_resource), 1};
    std::vector<ag_replay_segment> segments;
    std::vector<float> dashes;
    std::vector<uint8_t> pixels;
    void bind();
    size_t bytes() const;
};
struct Frame
{
    psx::recorder::Decoded header;
    uint32_t flags{};
    std::vector<Resource> resources;
    std::vector<ag_replay_command> commands;
    std::vector<ag_replay_resource> descriptors;
    ag_replay_snapshot snapshot() const;
    size_t bytes() const;
};
std::shared_ptr<const Frame> decode(const std::vector<uint8_t> &);
std::shared_ptr<const Frame> decode_instrument(const std::vector<uint8_t> &);
} // namespace psx::du
