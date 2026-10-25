#pragma once
#include <psx/audio/audio.hpp>
#include <array>
#include <atomic>

namespace psx_exporter {
// One producer per audited system boundary, one transport consumer. Independent of pose slots.
template<class T,std::size_t N> struct SampleRing {
    alignas(64) std::atomic<std::uint64_t> written{},read{};
    std::array<T,N> slots{};
    bool push(const T& v) noexcept {auto w=written.load(std::memory_order_relaxed),r=read.load(std::memory_order_acquire);if(w-r>=N)return false;slots[w%N]=v;written.store(w+1,std::memory_order_release);return true;}
    bool pop(T& v) noexcept {auto r=read.load(std::memory_order_relaxed),w=written.load(std::memory_order_acquire);if(r==w)return false;v=slots[r%N];read.store(r+1,std::memory_order_release);return true;}
};
// Native transport writes; the normal PSX audio path observes at most two bounded attempts.
// The ticket is acknowledged only after all whitelisted gains were refreshed in PSX's context.
struct OutputLease {
    std::atomic<std::uint64_t> version{},applied{};
    std::atomic<std::int64_t> expires{};
    std::atomic<std::uint32_t> groups{};
    void set(std::uint32_t mask,std::int64_t deadline) noexcept {version.fetch_add(1);groups.store(mask);expires.store(deadline);version.fetch_add(1);}
    std::uint64_t token(std::int64_t now,std::uint32_t capabilities) const noexcept {
        for(int i=0;i<2;++i){auto before=version.load();if(before&1)continue;auto mask=groups.load()&capabilities;auto expiry=expires.load();if(version.load()!=before)continue;return (before<<8)|(now<expiry?mask:0);}
        return 0;
    }
};
}
