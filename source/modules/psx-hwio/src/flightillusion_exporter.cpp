#include "hwio/flightillusion_exporter.hpp"
#include <psx/instructor/maximum_speed.hpp>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <thread>

namespace hwio::flightillusion {
namespace {
std::uint64_t unix_ms(){return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
std::uint64_t steady_ms(){return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
ExporterSource::Reader native_reader(){
    struct State {psx::instructor::NavigationDatabaseSelection main;psx::instructor::MaximumSpeedSelection speed;};
    return [state=std::make_shared<State>()](const ExporterRequest& request,std::uint64_t){
        const auto main=state->main.read(request.host,request.port);
        const auto sample=state->speed.read(main,unix_ms());
        return ExporterSample{sample.max_speed_kt,sample.sampled_unix_ms,sample.notice};
    };
}
}
struct ExporterSource::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    Reader reader;
    ExporterRequest request;
    ExporterSample sample;
    std::uint64_t generation{},expires{};
    bool stop{};
    std::thread worker;
    explicit Impl(Reader r):reader(r?std::move(r):native_reader()){}
    void poll(std::uint64_t utc,std::uint64_t ms,bool realtime=false){
        ExporterRequest current;std::uint64_t revision;
        {std::lock_guard lock(mutex);if(!request.active)return;current=request;revision=generation;}
        ExporterSample next;
        try {next=reader(current,utc);}
        catch(const std::exception& error){next.notice=error.what();}
        catch(...){next.notice="Exporter maximum-speed read failed";}
        // Identity resolution and file I/O can take time. Validate against read
        // completion, and deduct all elapsed age from the monotonic deadline.
        if(realtime){utc=unix_ms();ms=steady_ms();}
        if(next.knots&&(!std::isfinite(*next.knots)||*next.knots<=0||*next.knots>450||!next.notice.empty())){next.knots.reset();next.notice="Invalid exporter indicated limit";}
        std::uint64_t deadline{};
        if(next.knots){
            if(!next.sampled_unix_ms||next.sampled_unix_ms>utc||utc-next.sampled_unix_ms>=1000){next.knots.reset();next.notice="Exporter maximum speed is stale or future dated";}
            else deadline=ms+1000-(utc-next.sampled_unix_ms);
        }
        if(!next.knots&&next.notice.empty())next.notice="Exporter maximum speed unavailable";
        std::lock_guard lock(mutex);
        if(revision!=generation||stop)return;
        sample=std::move(next);expires=deadline;
    }
    void run(){
        std::unique_lock lock(mutex);
        while(!stop){
            if(!request.active){wake.wait(lock,[&]{return stop||request.active;});continue;}
            const auto revision=generation;
            lock.unlock();const auto utc=unix_ms(),ms=steady_ms();poll(utc,ms,true);lock.lock();
            wake.wait_for(lock,std::chrono::milliseconds(100),[&]{return stop||generation!=revision;});
        }
    }
};
ExporterSource::ExporterSource(Reader reader,bool threaded):impl_(std::make_unique<Impl>(std::move(reader))){if(threaded)impl_->worker=std::thread([this]{impl_->run();});}
ExporterSource::~ExporterSource(){
    {std::lock_guard lock(impl_->mutex);impl_->stop=true;}impl_->wake.notify_all();
    if(impl_->worker.joinable())impl_->worker.join();
}
void ExporterSource::configure(ExporterRequest request){
    {std::lock_guard lock(impl_->mutex);if(impl_->request==request)return;impl_->request=std::move(request);++impl_->generation;impl_->sample={};impl_->sample.notice="Waiting for current Main exporter maximum speed";impl_->expires=0;}
    impl_->wake.notify_all();
}
void ExporterSource::pump(std::uint64_t utc,std::uint64_t ms){impl_->poll(utc,ms);}
void ExporterSource::apply(Sources& sources,std::uint64_t utc,std::uint64_t ms)const{
    constexpr auto index=unsigned(Signal::limit_speed);
    sources.values[index].reset();sources.valid_until_ms[index]=0;
    std::lock_guard lock(impl_->mutex);
    const auto& request=impl_->request;const auto& sample=impl_->sample;
    auto session=request.epoch+1;if(!session)session=1;
    auto& notice=sources.unavailable[index];
    if(!request.active||!sources.controls_ready||sources.session!=session){notice="PSX Main source is not ready";return;}
    if(!sample.knots){notice=sample.notice;return;}
    // Repeated host snapshots cannot renew the exporter's observation. Both
    // wall and monotonic age are checked, including after a Windows clock change.
    if(sample.sampled_unix_ms>utc||utc-sample.sampled_unix_ms>=1000||ms>=impl_->expires){notice="Exporter maximum speed is stale or future dated";return;}
    sources.values[index]=sample.knots;sources.valid_until_ms[index]=impl_->expires;notice.clear();
}
}
