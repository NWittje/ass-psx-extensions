#pragma once
#include "hid.hpp"
#include <iterator>

namespace hwio {
// Non-owning acquisition boundary. Manufacturer workers retain their protocols,
// connections and generation rules; this view neither starts hardware nor writes
// outputs. Keep the provider alive until the acquisition owner has joined.
// Samples retain physical device/control identity, generation, sequence and QPC.
class InputProvider final {
    InputProvider()=default;
    void* context_{};
    std::size_t first_{},limit_{};
    bool (*pop_)(void*,Sample&){};
    std::size_t (*queued_)(const void*){};
    bool (*latest_)(const void*,std::size_t,std::size_t,Sample&){};
    void (*devices_)(void*,std::vector<DeviceInfo>&){};
public:
    // HID/PoKeys selectors finish at their producer's report marker. Other
    // adapters also support the existing bounded sample-service allowance.
    bool complete_reports{};
    template<class Provider>
    static InputProvider bind(Provider& provider,std::size_t first,std::size_t limit,
                              bool reports=false) noexcept {
        InputProvider view;
        view.context_=&provider;view.first_=first;view.limit_=limit;
        view.complete_reports=reports;
        view.pop_=[](void* p,Sample& sample){return static_cast<Provider*>(p)->edges.pop(sample);};
        view.queued_=[](const void* p){return std::size_t(static_cast<const Provider*>(p)->edges.size());};
        view.latest_=[](const void* p,std::size_t device,std::size_t control,Sample& sample){
            return static_cast<const Provider*>(p)->latest(device,control,sample);
        };
        view.devices_=[](void* p,std::vector<DeviceInfo>& result){
            auto devices=static_cast<Provider*>(p)->devices();
            result.insert(result.end(),std::make_move_iterator(devices.begin()),std::make_move_iterator(devices.end()));
        };
        return view;
    }
    bool contains(std::size_t device) const noexcept {return first_<=device&&device<limit_;}
    bool latest(std::size_t device,std::size_t control,Sample& sample) const {
        return contains(device)&&control<max_controls&&latest_(context_,device,control,sample);
    }
    bool pop(Sample& sample) const {return pop_(context_,sample);}
    std::size_t queued() const {return queued_(context_);}
    void append_devices(std::vector<DeviceInfo>& devices) const {devices_(context_,devices);}
    void discard_batch() const {
        // Freeze the amount being retired. A producer that keeps replenishing
        // its queue must not hold PSX recovery or other manufacturers forever.
        auto remaining=queued();Sample discarded;
        while(remaining--&&pop(discarded)){}
    }
};

template<std::size_t N>
bool provider_latest(const std::array<InputProvider,N>& providers,std::size_t device,
                     std::size_t control,Sample& sample) {
    for(const auto& provider:providers)if(provider.contains(device))
        return provider.latest(device,control,sample);
    return false;
}
} // namespace hwio
