#include "coordinator/environment_evidence.hpp"
#include "evidence_file_worker.hpp"
#include <array>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace coordinator {
namespace {
void put(std::uint8_t* p,std::uint64_t v,std::size_t count){for(std::size_t n=0;n<count;++n)p[n]=static_cast<std::uint8_t>(v>>(8*n));}
}
struct EnvironmentEvidence::Impl {
    struct Record {std::uint8_t type{},role{};std::uint32_t size{};std::int64_t time{};std::array<std::uint8_t,environment_wire::max_frame> bytes{};bool outgoing{};EnvironmentDispatch dispatch{};};
    mutable std::mutex mutex;std::condition_variable ready;EvidenceFileWorker file_owner;
    std::array<Record,512> queue{};std::size_t head{},size{};EnvironmentEvidenceStatus view{};
    bool begun{},stopping{};std::int64_t start{},end{};std::uint64_t frequency{};
    void fail(DWORD error){std::lock_guard<std::mutex> lock(mutex);view.active=view.complete=false;view.incomplete=true;view.error=error;stopping=true;size=0;}
    bool write(const void* p,DWORD n){DWORD count{};const bool written=WriteFile(file_owner.file(),p,n,&count,nullptr)!=FALSE;const auto error=written?ERROR_WRITE_FAULT:GetLastError();const bool ok=written&&count==n;std::lock_guard<std::mutex> lock(mutex);if(ok)view.bytes+=count;else {view.error=error;view.incomplete=true;view.active=false;stopping=true;}return ok;}
    void run(){
        {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return begun||stopping;});}
        std::array<std::uint8_t,24> header{{'P','X','W','C','A','1',0,0}};put(header.data()+8,frequency,8);put(header.data()+16,static_cast<std::uint64_t>(start),8);bool healthy=write(header.data(),static_cast<DWORD>(header.size()));
        while(healthy){Record r;{std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return size||stopping;});if(!size&&stopping)break;r=queue[head];head=(head+1)%queue.size();--size;}
            if(r.outgoing){environment_wire::Frame f;const bool encoded=encode_environment_dispatch(r.dispatch,static_cast<environment_wire::Role>(r.role),1,f);if(!encoded){std::lock_guard<std::mutex> lock(mutex);view.incomplete=true;view.error=ERROR_INVALID_DATA;view.active=false;stopping=true;end=r.time;continue;}r.size=static_cast<std::uint32_t>(f.size);std::copy(f.bytes.begin(),f.bytes.begin()+f.size,r.bytes.begin());}
            std::array<std::uint8_t,16> h{};put(h.data(),static_cast<std::uint64_t>(r.time),8);h[8]=r.type;h[9]=r.role;put(h.data()+12,r.size,4);
            healthy=write(h.data(),static_cast<DWORD>(h.size()))&&write(r.bytes.data(),r.size);
            std::lock_guard<std::mutex> lock(mutex);++view.records;if(view.bytes>128ull*1024*1024){view.incomplete=true;view.active=false;stopping=true;end=r.time;}
        }
        bool incomplete;std::int64_t cutoff;{std::lock_guard<std::mutex> lock(mutex);incomplete=view.incomplete||!begun;cutoff=end;}
        if(healthy){std::array<std::uint8_t,16> tail{};put(tail.data(),static_cast<std::uint64_t>(cutoff),8);tail[8]=incomplete?4:3;healthy=write(tail.data(),static_cast<DWORD>(tail.size()));}
        std::lock_guard<std::mutex> lock(mutex);view.active=false;view.complete=healthy&&!view.incomplete&&begun;
    }
};
EnvironmentEvidence::EnvironmentEvidence():impl_(std::make_unique<Impl>()){}
EnvironmentEvidence::~EnvironmentEvidence(){finish(0);join();}
bool EnvironmentEvidence::prepare(const std::wstring& name,std::uint64_t frequency){if(!frequency)return false;auto* p=impl_.get();
    return p->file_owner.prepare(name,[p,frequency]{std::lock_guard<std::mutex> lock(p->mutex);p->view={};p->begun=p->stopping=false;p->head=p->size=0;p->frequency=frequency;p->start=p->end=0;},[p]{p->run();},[p](DWORD error){p->fail(error);});
}
void EnvironmentEvidence::begin(std::int64_t now){std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->file_owner.done()||impl_->stopping)return;impl_->start=now;impl_->begun=impl_->view.active=true;impl_->ready.notify_one();}
void EnvironmentEvidence::record(std::uint8_t type,environment_wire::Role role,const std::uint8_t* data,std::size_t size,std::int64_t now){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->view.active)return;
    if(size>environment_wire::max_frame||impl_->size==impl_->queue.size()){++impl_->view.overflows;impl_->view.incomplete=true;impl_->view.active=false;impl_->stopping=true;impl_->end=now;impl_->ready.notify_one();return;}
    auto& r=impl_->queue[(impl_->head+impl_->size)%impl_->queue.size()];r.outgoing=false;r.type=type;r.role=static_cast<std::uint8_t>(role);r.time=now;r.size=static_cast<std::uint32_t>(size);std::copy(data,data+size,r.bytes.begin());++impl_->size;impl_->ready.notify_one();
}
void EnvironmentEvidence::dispatch(environment_wire::Role role,const EnvironmentDispatch& value,std::int64_t now,std::uint8_t type){std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->view.active)return;if(impl_->size==impl_->queue.size()){++impl_->view.overflows;impl_->view.incomplete=true;impl_->view.active=false;impl_->stopping=true;impl_->end=now;impl_->ready.notify_one();return;}auto& r=impl_->queue[(impl_->head+impl_->size)%impl_->queue.size()];r.outgoing=true;r.dispatch=value;r.type=type;r.role=static_cast<std::uint8_t>(role);r.time=now;++impl_->size;impl_->ready.notify_one();}
void EnvironmentEvidence::finish(std::int64_t now) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->view.active=false;if(!impl_->stopping)impl_->end=now;impl_->stopping=true;impl_->ready.notify_one();}
void EnvironmentEvidence::incomplete(std::uint32_t error,std::int64_t now) noexcept {std::lock_guard<std::mutex> lock(impl_->mutex);if(!impl_->view.active)return;impl_->view.incomplete=true;impl_->view.error=error;impl_->view.active=false;impl_->end=now;impl_->stopping=true;impl_->ready.notify_one();}
void EnvironmentEvidence::join(){impl_->file_owner.join();}
bool EnvironmentEvidence::done() const noexcept{return impl_->file_owner.done();}
bool EnvironmentEvidence::test_open_hook(void (*hook)(void*) noexcept,void* context){return impl_->file_owner.test_open_hook(hook,context);}
EnvironmentEvidenceStatus EnvironmentEvidence::status() const {std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->view;}
}
