#include "transport.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <bcrypt.h>

namespace psx::hardware {
namespace {
std::uint64_t clock_us() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
ConnectionPhase phase_of(HostPhase phase) noexcept {
    switch(phase){case HostPhase::discovering:return ConnectionPhase::discovering;
    case HostPhase::starting:return ConnectionPhase::starting;case HostPhase::ready:return ConnectionPhase::ready;
    case HostPhase::failed:return ConnectionPhase::failed;default:return ConnectionPhase::closed;}
}
const wchar_t* fault_text(HostFault fault) noexcept {
    switch(fault){case HostFault::discovery_timeout:return L"No compatible display replied within 5 seconds.";
    case HostFault::handshake_timeout:return L"Display session handshake timed out.";
    case HostFault::identity_mismatch:return L"Display identity, board, profile, role or artwork does not match.";
    case HostFault::peer_timeout:return L"No valid current-session device traffic for 3 seconds. Connect again.";
    case HostFault::transport:return L"Serial transport failed. Check the selected port and its owner.";
    default:return L"";}
}
}
struct HardwareConnection::Impl {
    detail::SerialTransport serial;
    detail::TransportOps io;
    HostCallbacks callbacks;
    HANDLE wake{};
    mutable std::mutex gate;
    ConnectionStatus snapshot{};
    DeviceBinding binding{};
    ProfileState latest{};
    std::uint64_t published{};
    bool requested{},active{};
    std::atomic_bool closing{},stopping{};
    std::thread worker;
    explicit Impl(HostCallbacks cb,detail::TransportOps override_io):io(override_io.open?override_io:serial.operations()),callbacks(cb){
        wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!wake)throw std::runtime_error("Cannot create connection wake event");
        try{worker=std::thread([this]{run();});}catch(...){CloseHandle(wake);throw;}
    }
    ~Impl(){stopping=true;closing=true;SetEvent(wake);if(worker.joinable())worker.join();CloseHandle(wake);}
    void update(const HostSession& host,bool owns,DWORD error=0){
        std::lock_guard lock(gate);snapshot.link=host.status();snapshot.owns_port=owns;
        snapshot.phase=closing?ConnectionPhase::closing:phase_of(snapshot.link.phase);
        if(error)snapshot.windows_error=error;
        snapshot.error.fill(0);const auto text=fault_text(snapshot.link.fault);std::copy_n(text,std::min(wcslen(text),snapshot.error.size()-1),snapshot.error.begin());
    }
    void run() noexcept {
        while(!stopping){
            WaitForSingleObject(wake,INFINITE);
            if(stopping)break;
            DeviceBinding selected;
            {std::lock_guard lock(gate);if(!requested)continue;selected=binding;requested=false;}
            HostSession host;DWORD error{};bool owns=false;
            if(!closing){
                owns=io.open(io.context,selected.candidate,error);
                if(owns){
                    std::array<std::uint8_t,12> random{};
                    if(BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){
                        host.transport_fault();error=ERROR_GEN_FAILURE;
                    }else host.start(selected.expected,callbacks,read64(random,0),read32(random,8),clock_us(),selected.publication);
                }else host.transport_fault();
            }
            std::uint64_t seen{};std::array<std::uint8_t,512> input{};
            while(owns&&!closing&&!stopping&&host.status().phase!=HostPhase::failed){
                ProfileState state{};bool changed=false;
                {std::lock_guard lock(gate);if(published!=seen){state=latest;seen=published;changed=true;}}
                if(changed)host.publish_latest(state);
                const auto read=io.read(io.context,input);
                if(read.state==detail::IoState::failed){error=read.error;host.transport_fault();break;}
                if(read.state==detail::IoState::completed&&read.count)host.receive(Bytes(input.data(),read.count),clock_us());
                auto now=clock_us();host.service(now);
                const auto bytes=host.pending_tx();
                if(host.status().phase!=HostPhase::failed&&!bytes.empty()){
                    const auto write=io.write(io.context,bytes);
                    if(write.state==detail::IoState::failed){error=write.error;host.transport_fault();break;}
                    if(write.state==detail::IoState::completed&&!host.consume_tx(write.count)){error=ERROR_INVALID_DATA;host.transport_fault();break;}
                }
                update(host,true);
                now=clock_us();const auto due=host.next_wake(now);
                const auto milliseconds=DWORD(std::clamp<std::uint64_t>((due-now+999)/1000,1,50));
                io.wait(io.context,wake,milliseconds);
            }
            if(owns)io.cancel_close(io.context);
            {std::lock_guard lock(gate);active=false;snapshot.link=host.status();snapshot.owns_port=false;snapshot.windows_error=error;
                snapshot.error.fill(0);const auto text=fault_text(snapshot.link.fault);std::copy_n(text,std::min(wcslen(text),snapshot.error.size()-1),snapshot.error.begin());
                if(closing||stopping){snapshot.phase=ConnectionPhase::closed;snapshot.link.phase=HostPhase::closed;}
                else snapshot.phase=ConnectionPhase::failed;
            }
        }
    }
};
HardwareConnection::HardwareConnection(HostCallbacks callbacks):HardwareConnection(callbacks,{}){}
HardwareConnection::HardwareConnection(HostCallbacks callbacks,detail::TransportOps io):impl_(std::make_unique<Impl>(callbacks,io)){}
HardwareConnection::~HardwareConnection()=default;
OpenResult HardwareConnection::open(const DeviceBinding& binding){
    if(binding.candidate.interface_path.empty()||!binding.expected.board||!binding.expected.profile||!binding.expected.revision||
       !binding.publication.minimum_interval_us||binding.publication.heartbeat_us<binding.publication.minimum_interval_us||
       !binding.publication.peer_timeout_us)return OpenResult::invalid;
    std::lock_guard lock(impl_->gate);if(impl_->active)return OpenResult::busy;
    impl_->binding=binding;impl_->snapshot={};impl_->snapshot.phase=ConnectionPhase::opening;
    impl_->published=0;impl_->latest={};impl_->closing=false;impl_->active=true;impl_->requested=true;
    SetEvent(impl_->wake);return OpenResult::accepted;
}
SendResult HardwareConnection::publish_latest(const ProfileState& state) noexcept {
    if(state.type<64||state.size>max_payload)return SendResult::invalid;
    std::lock_guard lock(impl_->gate);
    if(!impl_->active||impl_->closing)return SendResult::busy;
    impl_->latest=state;++impl_->published;SetEvent(impl_->wake);return SendResult::accepted;
}
ConnectionStatus HardwareConnection::status() const noexcept {std::lock_guard lock(impl_->gate);return impl_->snapshot;}
void HardwareConnection::close() noexcept {
    std::lock_guard lock(impl_->gate);impl_->closing=true;
    if(impl_->active)impl_->snapshot.phase=ConnectionPhase::closing;
    else impl_->snapshot.phase=ConnectionPhase::closed;
    SetEvent(impl_->wake);
}
}
