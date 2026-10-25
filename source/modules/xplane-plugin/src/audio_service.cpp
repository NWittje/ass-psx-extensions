#include "psx_plugin/audio_service.hpp"
#include "psx_plugin/audio_assets.hpp"
#include "psx_plugin/observation_client.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include <Windows.h>
#include <thread>
namespace psx::xp {
struct AudioService::Impl {
    struct Latest {AudioHost host{};audio::State state{};std::uint64_t revision{};};
    struct Command {bool edge{},stop{};audio::Control control{};audio::Transition transition{};std::uint64_t revision{};};
    TripleBuffer<Latest> incoming;
    TripleBuffer<audio::Status> outgoing;
    SpscFifo<Command,128> commands;
    std::atomic<std::uint64_t> fault_generation{};
    std::uint64_t published_revision{};bool published_follow{};
    HANDLE stop_event{};std::thread worker;
    ~Impl(){if(stop_event){SetEvent(stop_event);if(worker.joinable())worker.join();CloseHandle(stop_event);}}
    void run(void* borrowed) noexcept {
        FmodApi api;api.resolve(borrowed);AudioRenderer renderer(api,observation_qpc_frequency());
        std::uint64_t seen_fault{},asset_generation=~std::uint64_t{};std::array<char,65> asset_hash{};bool assets{};
        std::uint64_t consumed_revision{};Command pending{};bool have_pending{};
        while(WaitForSingleObject(stop_event,5)==WAIT_TIMEOUT){
            if(incoming.consume()){
                consumed_revision=incoming.reading().revision;
                auto host=incoming.reading().host;
                const bool changed=asset_generation!=host.aircraft_generation||asset_hash!=host.profile.sha256;
                if(changed){renderer.host(host); // Stops old instances before potentially expensive hashing.
                    asset_generation=host.aircraft_generation;asset_hash=host.profile.sha256;assets=qualify_audio_assets(host);}
                host.profile.audio_qualified=host.profile.audio_qualified&&assets;
                renderer.host(host);
                if(host.connected&&incoming.reading().state.audio_epoch)renderer.state(incoming.reading().state);
            }
            const auto fault=fault_generation.load(std::memory_order_acquire);
            if(fault!=seen_fault){seen_fault=fault;renderer.fault();}
            for(std::size_t i=0;i<128;++i){
                if(!have_pending)have_pending=commands.pop(pending);
                if(!have_pending||pending.revision>consumed_revision)break;
                if(pending.stop)renderer.stop_intent();
                else if(pending.edge)renderer.transition(pending.transition);else renderer.control(pending.control,observation_qpc());
                have_pending=false;
            }
            renderer.tick(observation_qpc());outgoing.writing()=renderer.status();outgoing.publish();
        }
        renderer.release();
    }
};
AudioService::AudioService()=default;
AudioService::~AudioService(){stop();}
bool AudioService::start(void* borrowed) noexcept {
    stop();try{auto next=std::make_unique<Impl>();next->stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!next->stop_event)return false;
        auto* const raw=next.get();raw->worker=std::thread([raw,borrowed]{raw->run(borrowed);});impl_=std::move(next);return true;
    }catch(...){return false;}
}
void AudioService::stop() noexcept {impl_.reset();}
void AudioService::submit(const AudioHost& host,const audio::State* state) noexcept {
    if(!impl_)return;auto& out=impl_->incoming.writing();out.host=host;out.state=state?*state:audio::State{};
    out.revision=++impl_->published_revision;impl_->incoming.publish();
    const bool follow=host.connected&&host.follow;
    if(impl_->published_follow&&!follow){Impl::Command cmd;cmd.stop=true;cmd.revision=impl_->published_revision;
        if(!impl_->commands.push(cmd))fault();}
    impl_->published_follow=follow;
}
bool AudioService::control(const audio::Control& c) noexcept {
    if(!impl_)return false;Impl::Command cmd;cmd.control=c;cmd.revision=impl_->published_revision;if(impl_->commands.push(cmd))return true;fault();return false;
}
bool AudioService::transition(const audio::Transition& t) noexcept {
    if(!impl_)return false;Impl::Command cmd;cmd.edge=true;cmd.transition=t;cmd.revision=impl_->published_revision;if(impl_->commands.push(cmd))return true;fault();return false;
}
void AudioService::fault() noexcept {if(impl_)impl_->fault_generation.fetch_add(1,std::memory_order_release);}
bool AudioService::poll(audio::Status& s) noexcept {if(!impl_||!impl_->outgoing.consume())return false;s=impl_->outgoing.reading();return true;}
}
