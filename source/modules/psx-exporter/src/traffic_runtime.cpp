#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <jni.h>
#include <psx_exporter/traffic_snapshot.hpp>
#include <psx_exporter/traffic_runtime.hpp>
#include <atomic>
#include <cstring>
#include <cmath>

namespace {
namespace t=psx::traffic;
struct Latest {
    std::array<t::Snapshot,3> slots{};unsigned back=0,front=1;std::atomic<unsigned> middle{2};
    void put(const t::Snapshot& value) noexcept {slots[back]=value;back=middle.exchange(back|4,std::memory_order_acq_rel)&3;}
    bool take(t::Snapshot& value) noexcept {if(!(middle.load(std::memory_order_acquire)&4))return false;front=middle.exchange(front,std::memory_order_acq_rel)&3;value=slots[front];return true;}
};
struct Runtime {
    Latest latest,diagnostic;void* source{};std::uint64_t created{},frequency{},sequence{},observed{};
    std::atomic<std::uint32_t> capability{},producer{};std::atomic<bool> initialized{},reader{},transport{},fault{};
    std::atomic<std::uint64_t> attempted{},accepted{},rejected{},elapsed{},maximum{};
} runtime;
std::uint64_t ticks() noexcept {LARGE_INTEGER v{};QueryPerformanceCounter(&v);return static_cast<std::uint64_t>(v.QuadPart);}
void measure(std::uint64_t start,bool accepted) noexcept {
    const auto duration=ticks()-start;runtime.attempted.fetch_add(1,std::memory_order_relaxed);
    (accepted?runtime.accepted:runtime.rejected).fetch_add(1,std::memory_order_relaxed);
    runtime.elapsed.fetch_add(duration,std::memory_order_relaxed);
    // Publication has exactly one qualified producer; no retry loop on native callbacks.
    const auto maximum=runtime.maximum.load(std::memory_order_relaxed);if(duration>maximum)runtime.maximum.store(duration,std::memory_order_relaxed);
}
}
namespace psx_exporter {
bool own_traffic_reader() noexcept {
    if(runtime.transport.load(std::memory_order_acquire))return true;
    bool idle=false;if(!runtime.reader.compare_exchange_strong(idle,true,std::memory_order_acquire))return false;
    runtime.transport.store(true,std::memory_order_release);runtime.reader.store(false,std::memory_order_release);return true;
}
bool take_traffic(t::Snapshot& value) noexcept {
    if(!runtime.transport.load(std::memory_order_acquire)||!runtime.capability.load(std::memory_order_acquire))return false;
    return runtime.latest.take(value)&&runtime.capability.load(std::memory_order_acquire);
}
void publish_traffic_diagnostic(const t::Snapshot& value) noexcept {runtime.diagnostic.put(value);}
std::uint32_t traffic_capabilities() noexcept {return runtime.capability.load(std::memory_order_acquire);}
psx::traffic_source::Reason traffic_reason() noexcept {return runtime.fault.load(std::memory_order_acquire)?psx::traffic_source::Reason::native_fault:traffic_capabilities()?psx::traffic_source::Reason::none:psx::traffic_source::Reason::unsupported;}
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_TrafficNative_abiVersion(JNIEnv*,jclass){return t::abi;}
extern "C" JNIEXPORT jlong JNICALL Java_psx_exporter_TrafficNative_intervalTicks(JNIEnv*,jclass){LARGE_INTEGER f{};return QueryPerformanceFrequency(&f)?(f.QuadPart+99)/100:0;}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_TrafficNative_initialize(JNIEnv* env,jclass,jobject source){
    if(!source||env->GetDirectBufferCapacity(source)!=sizeof(t::Snapshot)||runtime.capability.load()||runtime.producer.load())return JNI_FALSE;
    void* address=env->GetDirectBufferAddress(source);if(!address)return JNI_FALSE;
    FILETIME created{},exit{},kernel{},user{};LARGE_INTEGER frequency{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user)||!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return JNI_FALSE;
    runtime.source=address;runtime.created=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    runtime.frequency=static_cast<std::uint64_t>(frequency.QuadPart);runtime.initialized.store(true,std::memory_order_release);return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_TrafficNative_capabilities(JNIEnv*,jclass,jint mask){const auto cap=runtime.initialized.load(std::memory_order_acquire)?static_cast<std::uint32_t>(mask)&1u:0u;if(cap)runtime.fault.store(false,std::memory_order_release);runtime.capability.store(cap,std::memory_order_release);}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_TrafficNative_failed(JNIEnv*,jclass){runtime.fault.store(runtime.capability.load(std::memory_order_acquire)!=0||runtime.fault.load(std::memory_order_acquire),std::memory_order_release);runtime.capability.store(0,std::memory_order_release);}
extern "C" JNIEXPORT jboolean JNICALL Java_psx_exporter_TrafficNative_publish(JNIEnv* env,jclass,jobject source){
    const auto start=ticks();bool accepted=false;
    if(runtime.capability.load(std::memory_order_acquire)&&source&&env->GetDirectBufferCapacity(source)==sizeof(t::Snapshot)&&env->GetDirectBufferAddress(source)==runtime.source){
        const auto thread=GetCurrentThreadId();auto owner=runtime.producer.load(std::memory_order_relaxed);
        if(!owner){runtime.producer.compare_exchange_strong(owner,thread,std::memory_order_acq_rel);owner=runtime.producer.load(std::memory_order_relaxed);}
        if(owner==thread){
            t::Snapshot value{};std::memcpy(&value,runtime.source,sizeof(value));auto& h=value.header;
            bool valid=h.abi==1&&h.size==sizeof(value)&&h.slots==7&&h.anchor==1&&h.sequence>runtime.sequence&&h.lifecycle&&h.frame&&h.observed>0&&h.observed>=runtime.observed&&h.finished>=h.observed&&(h.flags&~15u)==0&&h.metadata_valid<=1;
            if(h.metadata_valid)valid=valid&&std::isfinite(h.rate)&&h.rate>=0&&h.native_delta>=0;
            valid=valid&&(((h.flags&1u)!=0)!=((h.flags&8u)!=0));
            for(std::size_t i=0;i<value.slots.size();++i){const auto& slot=value.slots[i];valid=valid&&slot.id==static_cast<std::int32_t>(i)&&slot.generation&&(slot.valid&~1023u)==0&&(slot.flags&~31u)==0&&!slot.reserved&&!slot.reserved_identity;
                for(std::size_t j=0;j<slot.values.size();++j)if(slot.valid&(1u<<j))valid=valid&&std::isfinite(slot.values[j]);
                if(slot.valid&1u)valid=valid&&std::abs(slot.values[0])<=1.5707963267948966;
                if(slot.valid&2u)valid=valid&&std::abs(slot.values[1])<=6.283185307179586;
                if(slot.valid&64u)valid=valid&&slot.values[6]>=0;
            }
            if(valid){h.process=GetCurrentProcessId();h.producer=1;h.process_created=runtime.created;h.frequency=runtime.frequency;h.reserved_identity=0;h.reserved=0;runtime.sequence=h.sequence;runtime.observed=h.observed;runtime.latest.put(value);accepted=true;}
        }
    }
    measure(start,accepted);return accepted?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT jint JNICALL Java_psx_exporter_TrafficNative_poll(JNIEnv* env,jclass,jobject destination){
    if(!destination||env->GetDirectBufferCapacity(destination)<sizeof(t::Snapshot)||!runtime.capability.load(std::memory_order_acquire))return 0;
    auto* address=env->GetDirectBufferAddress(destination);if(!address||address==runtime.source)return 0;
    bool idle=false;if(!runtime.reader.compare_exchange_strong(idle,true,std::memory_order_acquire))return 0;
    t::Snapshot value{};const bool current=(runtime.transport.load(std::memory_order_acquire)?runtime.diagnostic:runtime.latest).take(value)&&runtime.capability.load(std::memory_order_acquire);if(current)std::memcpy(address,&value,sizeof(value));runtime.reader.store(false,std::memory_order_release);return current?sizeof(value):0;
}
extern "C" JNIEXPORT void JNICALL Java_psx_exporter_TrafficNative_stats(JNIEnv* env,jclass,jobject destination){
    if(!destination||env->GetDirectBufferCapacity(destination)<64)return;auto* address=env->GetDirectBufferAddress(destination);if(!address)return;
    const std::array<std::uint64_t,8> values{{runtime.attempted.load(),runtime.accepted.load(),runtime.rejected.load(),runtime.elapsed.load(),runtime.maximum.load(),runtime.frequency,runtime.capability.load(),runtime.producer.load()}};
    std::memcpy(address,values.data(),sizeof(values));
}
