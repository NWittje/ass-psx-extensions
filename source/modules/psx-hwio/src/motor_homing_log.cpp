#include "hwio/motor_homing_log.hpp"
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstring>

namespace hwio {
namespace {
constexpr std::size_t log_limit=2*1024*1024;
template<std::size_t N> void text(char (&out)[N],std::string_view value) noexcept {
    const auto size=std::min(value.size(),N-1);if(size)std::memcpy(out,value.data(),size);out[size]=0;
    for(std::size_t i=0;i<size;++i)if(out[i]=='\n' || out[i]=='\r')out[i]=' ';
}
}
struct MotorHomingLog::Impl {
    struct Event {
        enum Kind {start,reading,phase} kind{};
        unsigned motor{},axis{};MotorHomingSample sample;
        char label[16]{},description[320]{};bool terminal{};
    };
    std::wstring directory;mutable std::mutex mutex;std::condition_variable wake;
    std::array<Event,256> queue;std::size_t head{},count{};std::uint64_t lost{};
    unsigned selected{};bool active{},closing{};std::uint64_t last_sample{};
    char last_phase[16]{};MotorHomingSample latest;std::string fault;std::thread writer;
    explicit Impl(std::wstring path):directory(std::move(path)),writer([this]{run();}){}
    ~Impl(){{std::lock_guard lock(mutex);closing=true;}wake.notify_one();writer.join();}
    void push(Event event) noexcept {
        if(count==queue.size()){++lost;return;}queue[(head+count)%queue.size()]=event;++count;wake.notify_one();
    }
    void run() noexcept {
        std::ofstream out;std::uint64_t origin{};bool capped{};
        try {
            for(;;){Event event;std::uint64_t gaps{};
                {std::unique_lock lock(mutex);wake.wait(lock,[&]{return closing || count;});if(!count && closing)break;
                    event=queue[head];head=(head+1)%queue.size();--count;gaps=lost;lost=0;}
                if(event.kind==Event::start){out.close();out.clear();capped=false;origin=event.sample.stamp_ms;
                    std::filesystem::create_directories(directory);out.open(std::filesystem::path(directory)/L"last-motor-homing.csv",std::ios::binary|std::ios::trunc);
                    if(!out)throw std::runtime_error("Cannot open last-motor-homing.csv");
                    constexpr const char* axes[]{"Throttle 1","Throttle 2","Throttle 3","Throttle 4","Speedbrake","TE flap left outboard","TE flap left inboard","TE flap right inboard","TE flap right outboard","Trim left","Trim right"};
                    out<<"# HWIO last motor homing / format 5 / interval 500 ms / axis "<<(event.axis<11?axes[event.axis]:"Unknown")<<" ("<<event.axis<<") / motor "<<event.motor<<" / "<<event.description<<'\n'
                       <<"# Phase descriptions and errors are single-sentence comment records; percentages are unclamped.\n# Speed is the requested signed position-coordinate limit, not measured shaft velocity; software loops use selected-sensor counts.\n"
                       <<"elapsed_ms,phase,speed_counts_s,acceleration_counts_s2,motor_counts,target_counts,axis_raw,axis_percent,raw_low,raw_high,sensor_resolution_percent,endpoint_band_percent,motor_age_ms,sensor_age_ms,motor_valid,sensor_valid,engaged,motor_generation,sensor_generation,calibration_revision,polarity,sensor_fraction_per_count,measured_velocity_counts_s,destination_distance_fraction,braking_margin_fraction,command_stage,requested_velocity_counts_s,progress_sensor_fraction,progress_encoder_counts,progress_age_ms,guard_target_counts,guard_decision,configuration_revision,coordinate_revision,drive_state,release_stage,sensor_response_limit_counts,sensor_codes,sensor_buffer_raw,jitter_raw,probe_excursion_raw\n"<<std::setprecision(12);
                }
                if(!out || capped)continue;
                const auto elapsed=event.sample.stamp_ms>=origin?event.sample.stamp_ms-origin:0;
                if(gaps)out<<"# "<<elapsed<<" error: Homing log queue lost "<<gaps<<" records.\n";
                if(event.kind==Event::phase){out<<"# "<<elapsed<<' '<<event.label<<": "<<event.description;
                    const auto length=std::strlen(event.description);if(!length || event.description[length-1]!='.')out<<'.';out<<'\n';
                }else if(event.kind==Event::reading){const auto& s=event.sample;
                    out<<elapsed<<','<<event.label<<','<<s.speed<<','<<s.acceleration<<','<<s.position<<','<<s.target<<',';
                    if(s.raw_valid)out<<s.raw;out<<','<<s.sensor*100<<',';if(s.raw_valid)out<<s.raw_low;out<<',';if(s.raw_valid)out<<s.raw_high;
                    out<<','<<s.resolution*100<<','<<s.endpoint_band*100<<','<<s.motor_age_ms<<','<<s.sensor_age_ms<<','<<s.motor_valid<<','<<s.sensor_valid<<','<<s.engaged<<','<<s.motor_generation<<','<<s.sensor_generation<<','<<s.calibration_revision<<','<<s.polarity<<','<<s.scale<<','<<s.measured_velocity<<','<<s.remaining<<','<<s.braking_margin<<','<<s.command_status<<','<<s.requested_velocity<<','<<s.progress_sensor<<','<<s.progress_encoder<<','<<s.progress_age<<','<<s.guard_target<<','<<s.guard_decision<<','<<s.configuration_revision<<','<<s.coordinate_revision<<','<<s.drive_state<<','<<s.release_stage<<','<<s.response_limit<<','<<s.codes<<','<<s.buffer<<','<<s.jitter<<','<<s.probe_excursion<<'\n';
                }
                if(out.tellp()>=std::streampos(log_limit)){out<<"# "<<elapsed<<" error: Homing log reached its 2 MiB limit and further records are omitted.\n";capped=true;}
                out.flush();if(!out)throw std::runtime_error("Cannot write last-motor-homing.csv");
            }
        }catch(const std::exception& e){std::lock_guard lock(mutex);fault=e.what();active=false;}
        catch(...){std::lock_guard lock(mutex);fault="Homing log writer failed";active=false;}
    }
};
MotorHomingLog::MotorHomingLog(std::wstring directory):impl_(std::make_unique<Impl>(std::move(directory))){}
MotorHomingLog::~MotorHomingLog()=default;
void MotorHomingLog::begin(unsigned motor,unsigned axis,std::string_view device,const MotorHomingSample& sample) noexcept {
    auto& p=*impl_;std::lock_guard lock(p.mutex);p.head=p.count=0;p.lost=0;p.selected=motor;p.active=p.fault.empty();p.latest=sample;p.last_sample=sample.stamp_ms;p.last_phase[0]=0;
    Impl::Event e;e.kind=Impl::Event::start;e.motor=motor;e.axis=axis;e.sample=sample;text(e.description,device);p.push(e);
}
void MotorHomingLog::update(unsigned motor,const MotorHomingSample& sample,std::string_view phase,std::string_view description,bool terminal) noexcept {
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(!p.active || p.selected!=motor)return;p.latest=sample;
    const bool changed=phase!=p.last_phase;
    if(changed){Impl::Event e;e.kind=Impl::Event::phase;e.sample=sample;text(e.label,phase);text(e.description,description);e.terminal=terminal;p.push(e);text(p.last_phase,phase);}
    const bool periodic=sample.stamp_ms>=p.last_sample+500;
    if(changed || terminal || periodic){Impl::Event e;e.kind=Impl::Event::reading;e.sample=sample;text(e.label,phase);p.push(e);if(periodic)p.last_sample=sample.stamp_ms;}
    if(terminal)p.active=false;
}
void MotorHomingLog::receipt(unsigned motor,const MotorHomingSample& sample,std::string_view description) noexcept {
    auto& p=*impl_;std::lock_guard lock(p.mutex);if(p.selected!=motor || !p.fault.empty())return;
    Impl::Event e;e.kind=Impl::Event::phase;e.sample=sample;text(e.label,"release");text(e.description,description);p.push(e);e.kind=Impl::Event::reading;p.push(e);
}
void MotorHomingLog::finish(unsigned motor,std::string_view description,bool fault) noexcept {
    auto& p=*impl_;MotorHomingSample sample;
    {std::lock_guard lock(p.mutex);if(!p.active || p.selected!=motor)return;sample=p.latest;}sample.speed=0;
    update(motor,sample,fault?"error":"stopped",description,true);
}
std::string MotorHomingLog::error()const{auto& p=*impl_;std::lock_guard lock(p.mutex);return p.fault;}
}
