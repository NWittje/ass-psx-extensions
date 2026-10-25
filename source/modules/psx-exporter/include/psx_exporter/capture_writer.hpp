#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace psx::recorder {
struct CaptureWrite {
    std::uint32_t kind{};
    std::uint64_t sequence{};
    std::vector<std::uint8_t> bytes;
};

// One collector producer and one disk consumer. The budget includes the active write.
// The producer never waits for the consumer, and no file I/O occurs on its thread.
template<class Sink> class CaptureWriter {
public:
    static constexpr std::uint64_t byte_limit=32ULL*1024*1024;
    static constexpr std::uint64_t entry_limit=128;
    struct Stats {
        std::uint64_t peak_bytes{},peak_entries{},overflows{},submitted{},written{},discarded{},max_write_ticks{},writes_over_250ms{};
    };
private:
    Sink& sink_;const std::uint64_t frequency_;HANDLE wake_{};
    std::array<CaptureWrite,entry_limit> ring_{};
    std::atomic<std::uint64_t> head_{},tail_{},owned_bytes_{},owned_entries_{};
    std::atomic<bool> closing_{},failed_{};
    std::thread worker_;Stats stats_{};std::string error_;
    static std::uint64_t ticks(){LARGE_INTEGER now{};QueryPerformanceCounter(&now);return static_cast<std::uint64_t>(now.QuadPart);}
    void run() noexcept {
        for(;;){
            const auto tail=tail_.load(std::memory_order_relaxed);
            if(tail==head_.load(std::memory_order_acquire)){
#ifdef PSX_CAPTURE_WRITER_TEST_EMPTY_HOOK
                PSX_CAPTURE_WRITER_TEST_EMPTY_HOOK(closing_);
#endif
                // closing may become visible after the earlier empty observation.
                // Its producer publishes the final head before setting closing.
                if(closing_.load(std::memory_order_acquire)){
                    if(tail==head_.load(std::memory_order_acquire))break;
                    continue;
                }
                WaitForSingleObject(wake_,100);continue;
            }
            auto item=std::move(ring_[tail%entry_limit]);
            tail_.store(tail+1,std::memory_order_release);
            const auto charge=item.bytes.capacity();
            if(!failed_.load(std::memory_order_relaxed)){
                const auto start=ticks();
                try{sink_.write(item);stats_.written++;}
                catch(const std::exception& failure){error_=failure.what();failed_.store(true,std::memory_order_release);stats_.discarded++;}
                catch(...){error_="capture storage worker exception";failed_.store(true,std::memory_order_release);stats_.discarded++;}
                const auto elapsed=ticks()-start;
                if(elapsed>stats_.max_write_ticks)stats_.max_write_ticks=elapsed;
                stats_.writes_over_250ms+=elapsed>frequency_/4;
            }else stats_.discarded++;
            // Free the active payload before releasing its ownership charge.
            std::vector<std::uint8_t>().swap(item.bytes);
            owned_bytes_.fetch_sub(charge,std::memory_order_release);
            owned_entries_.fetch_sub(1,std::memory_order_release);
        }
    }
public:
    CaptureWriter(Sink& sink,std::uint64_t frequency):sink_(sink),frequency_(frequency){
        wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!wake_)throw std::runtime_error("capture writer event unavailable");
        try{worker_=std::thread([this](){run();});}catch(...){CloseHandle(wake_);throw;}
    }
    CaptureWriter(const CaptureWriter&)=delete;
    CaptureWriter& operator=(const CaptureWriter&)=delete;
    ~CaptureWriter(){finish();CloseHandle(wake_);}
    bool enqueue(CaptureWrite&& item){
        if(failed_.load(std::memory_order_acquire)||closing_.load(std::memory_order_acquire))return false;
        const auto charge=static_cast<std::uint64_t>(item.bytes.capacity());
        const auto head=head_.load(std::memory_order_relaxed);
        if(charge>byte_limit||owned_bytes_.load(std::memory_order_acquire)>byte_limit-charge||
           owned_entries_.load(std::memory_order_acquire)>=entry_limit||head-tail_.load(std::memory_order_acquire)>=entry_limit){stats_.overflows++;return false;}
        const auto bytes=owned_bytes_.fetch_add(charge,std::memory_order_acq_rel)+charge;
        const auto entries=owned_entries_.fetch_add(1,std::memory_order_acq_rel)+1;
        if(bytes>stats_.peak_bytes)stats_.peak_bytes=bytes;
        if(entries>stats_.peak_entries)stats_.peak_entries=entries;
        ring_[head%entry_limit]=std::move(item);
        stats_.submitted++;head_.store(head+1,std::memory_order_release);SetEvent(wake_);return true;
    }
    bool failed()const{return failed_.load(std::memory_order_acquire);}
    void finish(){closing_.store(true,std::memory_order_release);SetEvent(wake_);if(worker_.joinable())worker_.join();}
    // Called only after finish, so worker-owned fields need no timing-path locks.
    const Stats& stats()const{return stats_;}
    const std::string& error()const{return error_;}
    std::uint64_t owned_bytes()const{return owned_bytes_.load(std::memory_order_acquire);}
    std::uint64_t owned_entries()const{return owned_entries_.load(std::memory_order_acquire);}
};
}
