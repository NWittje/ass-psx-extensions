#pragma once
#include "fds_modules.hpp"
namespace hwio::fds {
constexpr bool identification_on(std::uint64_t elapsed_ms) noexcept {return (elapsed_ms/1000)%2==0;}
std::string context_encode(const ModuleConfiguration&,const ButtonConfiguration&,const CockpitConfiguration&,std::span<const Device>);
struct ReplayResult {std::uint64_t entries{},contexts{},inputs{},parsed{},rejected{},bitmap_packets{},bitmap_edges{},raw_analogue{};std::vector<std::string> commands,outputs,faults;};
class ReplaySession {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    ReplaySession(std::uint64_t,const ModuleLibrary&);~ReplaySession();
    void append(const TraceEntry&);void finish();ReplayResult take();
};
ReplayResult replay(std::span<const TraceEntry>,std::uint64_t frequency,const ModuleLibrary&);
std::string sha256_file(const std::filesystem::path&);
}
