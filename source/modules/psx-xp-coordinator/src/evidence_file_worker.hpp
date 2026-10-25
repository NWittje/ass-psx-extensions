#pragma once
#include "coordinator/win_handle.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace coordinator {
// A single sidecar file owner. UI preparation can reject a busy owner without
// waiting for cloud filesystem I/O. Completion includes CloseHandle, so the
// original queue, checkpoint and buffers cannot be replaced while I/O owns them.
class EvidenceFileWorker {
    std::mutex lifecycle_;
    std::thread worker_;
    std::atomic<bool> done_{true};
    WinHandle file_;
    void (*before_open_)(void*) noexcept{};
    void* before_open_context_{};
public:
    bool done() const noexcept {return done_.load(std::memory_order_acquire);}
    HANDLE file() const noexcept {return file_.get();} // File worker only.
    template<class Initialize,class Run,class Fail>
    bool prepare(const std::wstring& path,Initialize initialize,Run run,Fail fail) {
        if(!done())return false;
        std::unique_lock<std::mutex> owner(lifecycle_,std::try_to_lock);
        if(!owner.owns_lock()||!done())return false;
        if(worker_.joinable())worker_.join(); // Only a confirmed completed owner.
        try {
            initialize();
            const auto hook=before_open_;const auto context=before_open_context_;
            done_.store(false,std::memory_order_release);
            worker_=std::thread([this,path,run,fail,hook,context]{
                try {
                    if(hook)hook(context);
                    file_.reset(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
                    if(file_.valid())run();else fail(GetLastError());
                }catch(...){fail(ERROR_UNHANDLED_EXCEPTION);}
                // Closing can also wait on the filesystem. Never hold the queue
                // mutex, detach the owner or publish done before it returns.
                file_.reset();done_.store(true,std::memory_order_release);
            });
            return true;
        }catch(...){fail(ERROR_NOT_ENOUGH_MEMORY);done_.store(true,std::memory_order_release);return false;}
    }
    void join(){std::lock_guard<std::mutex> owner(lifecycle_);if(worker_.joinable())worker_.join();}
    bool test_open_hook(void (*hook)(void*) noexcept,void* context) {
        std::unique_lock<std::mutex> owner(lifecycle_,std::try_to_lock);
        if(!owner.owns_lock()||!done())return false;
        before_open_=hook;before_open_context_=context;return true;
    }
};
}
