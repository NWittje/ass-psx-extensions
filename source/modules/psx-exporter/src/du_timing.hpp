#pragma once
#include <cstdint>
namespace psx::du::timing {
// Fixed primitive callback; optional diagnostics cannot propagate exceptions.
void event(unsigned producer,unsigned stage,unsigned unit,uint64_t generation,
           uint64_t frame,uint64_t paint,uint64_t value=0) noexcept;
}
