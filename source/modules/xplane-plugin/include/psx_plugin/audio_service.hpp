#pragma once
#include "audio_renderer.hpp"
#include <memory>
namespace psx::xp {
class AudioService final {
public:
    AudioService();~AudioService();
    AudioService(const AudioService&)=delete;AudioService& operator=(const AudioService&)=delete;
    bool start(void* borrowed_studio) noexcept; // SDK lifecycle; creates worker, no FMOD calls.
    void stop() noexcept; // SDK lifecycle only; join before borrowed banks/system can retire.
    void submit(const AudioHost&,const audio::State*) noexcept; // SDK frame: fixed copies only.
    bool control(const audio::Control&) noexcept;
    bool transition(const audio::Transition&) noexcept;
    void fault() noexcept;
    bool poll(audio::Status&) noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
