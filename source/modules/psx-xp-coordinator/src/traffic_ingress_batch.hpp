#pragma once
#include <Windows.h>
#include <cstddef>

namespace coordinator::detail {
enum class TrafficReadResult {complete,pending,failed};
enum class TrafficConsumeResult {consumed,yield,failed};

// A retained pending completion counts toward the same eight-object budget
// as an immediate read. The byte threshold yields after a completed object;
// the caller's one-PVX1-frame size check establishes the strict byte bound.
// Hello yields to the caller's observing handshake before another object.
// Every terminal turn returns control/output progress to the service worker.
template<class Read,class Consume>
bool drain_remote_traffic_ingress(Read&& read,Consume&& consume) {
    std::size_t bytes{};
    for(unsigned completions=0;completions<8&&bytes<16384;++completions){
        DWORD count{};const auto result=read(count);
        if(result==TrafficReadResult::pending)return true;
        if(result==TrafficReadResult::failed)return false;
        const auto consumed=consume(count);
        if(consumed==TrafficConsumeResult::failed)return false;
        bytes+=count;
        if(consumed==TrafficConsumeResult::yield)return true;
    }
    return true;
}
}
