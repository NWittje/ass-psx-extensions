#pragma once
#include <psx/scenery/protocol.hpp>
#include <filesystem>
#include <memory>
namespace psx::xp {
// File/package observations only; never calls placement, probes or other SDK APIs.
class SceneryCoverage final {
public:
    explicit SceneryCoverage(std::filesystem::path root);
    ~SceneryCoverage();
    scenery::Result inspect(const scenery::Query&) noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
