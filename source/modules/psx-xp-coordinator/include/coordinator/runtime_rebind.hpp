#pragma once
namespace coordinator {
// UI/timer owned. File and transport workers retain their storage until their
// actual completion; a stop request alone never admits a synchronous UI join.
class DeferredRuntimeRebind {
    bool pending_{},draining_{};
public:
    bool pending() const noexcept {return pending_;}
    // Cancelling reconnect does not undo the stop already posted to services.
    // A later explicit Connect must finish that drain even for original settings.
    bool required(bool settings_changed) const noexcept {return settings_changed||draining_;}
    void cancel() noexcept {pending_=false;}
    template<class Runtime>void request(Runtime& runtime) noexcept {
        if(pending_)return;
        pending_=true;if(!draining_){draining_=true;runtime.request_stop();}
    }
    template<class Runtime,class Apply>bool advance(Runtime& runtime,Apply apply) {
        if(!pending_||!runtime.stopped())return false;
        pending_=false;runtime.join();draining_=false;apply();return true;
    }
};
}
