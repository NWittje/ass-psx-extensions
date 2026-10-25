#pragma once
#include "trim/state.hpp"
namespace trim {
class Application {
public:
    Application(psx::hardware::Descriptor,Role) noexcept;
    Application(const Application&)=delete;
    Application& operator=(const Application&)=delete;
    psx::hardware::HardwareLink& link() noexcept {return link_;}
    const Observation& observation() const noexcept {return observation_;}
    void report(std::uint32_t rendered_session,std::uint32_t rendered_sequence,bool has_rendered,
                bool panel_fault,Mode mode,std::uint32_t heap_min,std::uint32_t dma_largest,
                std::uint32_t uart_errors,std::uint32_t comm_stack,std::uint32_t render_stack) noexcept;
private:
    static bool message(void*,const psx::hardware::Message&,std::uint64_t) noexcept;
    static void session(void*,std::uint32_t,bool) noexcept;
    Role role_;
    Observation observation_{};
    psx::hardware::HardwareLink link_;
};
}
