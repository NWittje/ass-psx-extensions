#pragma once
#include <Windows.h>
#include <cstddef>

namespace coordinator::detail {
enum class TerrainReadResult {complete,pending,failed};

// One remote object is one read completion. A completion retained from the
// preceding turn consumes the same budget as an immediately available object.
// Returning to the caller preserves an output/control opportunity every turn.
template<class Read,class Consume>
bool drain_remote_terrain_ingress(Read&& read,Consume&& consume) {
    std::size_t bytes{};
    for(unsigned completions=0;completions<8&&bytes<16384;++completions){
        DWORD count{};const auto result=read(count);
        if(result==TerrainReadResult::pending)return true;
        if(result==TerrainReadResult::failed||!consume(count))return false;
        bytes+=count;
    }
    return true;
}
}
