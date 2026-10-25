#include "hwio/motor_following_log.hpp"
#include "hwio/motor_bench.hpp"
#include <array>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <Windows.h>
namespace hwio {
struct MotorFollowingLog::Impl {
    static constexpr unsigned samples=301,slots=motor_slots;
    struct History {std::array<MotorFollowingTrace,samples> data{};unsigned begin{},count{};std::uint64_t last{};bool major{};};
    struct Fault {std::array<MotorFollowingTrace,samples> data{};unsigned count{};char cause[256]{};};
    std::array<History,slots> histories;std::array<Fault,4> queue;
    std::mutex mutex,files;std::condition_variable wake;bool stop{},enabled{true};unsigned head{},count{},next{};std::wstring directory;std::thread writer;
    explicit Impl(std::wstring d):directory(std::move(d)),writer([this]{run();}){}
    ~Impl(){{std::lock_guard lock(mutex);stop=true;}wake.notify_one();writer.join();}
    void run()noexcept {try {for(;;){Fault fault;unsigned index{};{std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop || count;});if(!count && stop)return;fault=queue[head];head=(head+1)%4;--count;index=next++%4;}
        std::lock_guard lock(files);std::filesystem::create_directories(directory);const auto path=std::filesystem::path(directory)/(L"motor-following-fault-"+std::to_wstring(index)+L".csv"),stage=std::filesystem::path(path.wstring()+L".tmp");
        std::ofstream out(stage,std::ios::binary|std::ios::trunc);out<<"# HWIO following fault / format 8 / preceding 30 seconds / interval 100 ms\n# "<<fault.cause<<"\n";
        out<<"ms,motor,phase,encoder,raw,raw_low,raw_high,sensor_fraction,expected_encoder,desired_raw,accepted_encoder,velocity,acceleration,measured_velocity,correction,error_limit,error_ms,response_ms,lag_ms,response_counts,generation,sensor_generation,coordinate,calibration,accepted_id,accepted_ms,expected_sequence,expected_ms,sensor_ms,encoder_ms,submissions,coalesced,overflows,sdk_code,drive,valid,expected_event,sensor_codes,sensor_buffer_raw,jitter_raw,software_position,reference_position,reference_velocity,reference_acceleration,requested_normalized_drive,accepted_normalized_drive,reported_drive,reported_drive_valid,psx_demand_fraction,psx_demand_sequence,psx_demand_valid,map_first_encoder,map_first_raw,map_second_encoder,map_second_raw,map_valid,nominal_encoder,compensated_encoder,submitted_encoder,accepted_physical_raw,accepted_velocity,accepted_acceleration,correcting,encoder_attained,sensor_sequence,arrival_after_ms,arrival_after_sequence,tracking_range,tracking_error,tracking_origin,tracking_progress,tracking_direction,tracking_started_ms,tracking_elapsed_ms,tracking_active,physical_hold,episode_anchor,target_change,edge,request_direction,retarget_kind,stop_reason,drive_event,pending_command,pending_limits,pending_release,correction_service_ms\n"<<std::setprecision(17);
        for(unsigned n=0;n<fault.count;++n){const auto& s=fault.data[n];out<<s.ms<<','<<s.motor<<','<<s.phase<<','<<s.encoder<<','<<s.raw<<','<<s.raw_low<<','<<s.raw_high<<','<<s.sensor<<','<<s.expected<<','<<s.desired<<','<<s.accepted<<','<<s.velocity<<','<<s.acceleration<<','<<s.measured_velocity<<','<<s.correction<<','<<s.error_limit<<','<<s.error_ms<<','<<s.response_ms<<','<<s.lag_ms<<','<<s.response_counts<<','<<s.generation<<','<<s.sensor_generation<<','<<s.coordinate<<','<<s.calibration<<','<<s.accepted_id<<','<<s.accepted_ms<<','<<s.expected_sequence<<','<<s.expected_ms<<','<<s.sensor_ms<<','<<s.encoder_ms<<','<<s.submissions<<','<<s.coalesced<<','<<s.overflows<<','<<s.sdk_code<<','<<s.drive<<','<<s.valid<<','<<s.expected_event<<','<<s.codes<<','<<s.buffer<<','<<s.jitter<<','<<s.software_position<<','<<s.reference<<','<<s.reference_velocity<<','<<s.reference_acceleration<<','<<s.requested_drive<<','<<s.accepted_drive<<','<<s.reported_drive<<','<<s.reported_drive_valid<<','<<s.demand_fraction<<','<<s.demand_sequence<<','<<s.demand_valid<<','<<s.first_count<<','<<s.first_raw<<','<<s.second_count<<','<<s.second_raw<<','<<s.map_valid<<','<<s.nominal_target<<','<<s.compensated_target<<','<<s.submitted_target<<','<<s.accepted_raw<<','<<s.accepted_velocity<<','<<s.accepted_acceleration<<','<<s.correcting<<','<<s.encoder_attained<<','<<s.sensor_sequence<<','<<s.arrival_after_ms<<','<<s.arrival_after_sequence<<','<<s.tracking_range<<','<<s.tracking_error<<','<<s.tracking_origin<<','<<s.tracking_progress<<','<<s.tracking_direction<<','<<s.tracking_started<<','<<s.tracking_elapsed<<','<<s.tracking_active<<','<<s.physical_hold<<','<<s.episode_anchor<<','<<s.target_change<<','<<s.edge<<','<<s.request_direction<<','<<s.retarget_kind<<','<<s.stop_reason<<','<<s.drive_event<<','<<s.pending_command<<','<<s.pending_limits<<','<<s.pending_release<<','<<s.correction_service_ms<<'\n';}
        out.flush();if(!out || out.tellp()>std::streampos(2*1024*1024))continue;out.close();MoveFileExW(stage.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }}catch(...) {}}
};
MotorFollowingLog::MotorFollowingLog(std::wstring d):impl_(std::make_unique<Impl>(std::move(d))){}
MotorFollowingLog::~MotorFollowingLog()=default;
void MotorFollowingLog::enabled(bool on)noexcept {std::lock_guard lock(impl_->mutex);impl_->enabled=on;}
void MotorFollowingLog::sample(MotorFollowingTrace s,std::string_view cause,bool major)noexcept {
    auto& p=*impl_;if(s.motor>=p.slots)return;std::lock_guard lock(p.mutex);if(!p.enabled)return;auto& h=p.histories[s.motor];
    const bool fault=major && !h.major;h.major=major;if(!fault && h.count && s.ms<h.last+100)return;h.last=s.ms;
    while(h.count && s.ms>=h.data[h.begin].ms && s.ms-h.data[h.begin].ms>30000){h.begin=(h.begin+1)%p.samples;--h.count;}
    if(h.count==p.samples){h.begin=(h.begin+1)%p.samples;--h.count;}h.data[(h.begin+h.count++)%p.samples]=s;
    if(fault){if(p.count==p.queue.size()){p.head=(p.head+1)%4;--p.count;}auto& f=p.queue[(p.head+p.count++)%4];f.count=h.count;for(unsigned n=0;n<h.count;++n)f.data[n]=h.data[(h.begin+n)%p.samples];const auto bytes=std::min(cause.size(),sizeof(f.cause)-1);std::memcpy(f.cause,cause.data(),bytes);f.cause[bytes]=0;p.wake.notify_one();}
}
void MotorFollowingLog::export_to(const std::filesystem::path& destination){auto& p=*impl_;std::lock_guard lock(p.files);for(unsigned n=0;n<4;++n){const auto name=L"motor-following-fault-"+std::to_wstring(n)+L".csv";const auto source=std::filesystem::path(p.directory)/name;if(!std::filesystem::exists(source))continue;if(std::filesystem::file_size(source)>2*1024*1024)throw std::runtime_error("Motor fault context exceeds 2 MiB");std::ifstream in(source,std::ios::binary);std::ofstream out(destination/name,std::ios::binary|std::ios::trunc);out<<in.rdbuf();if(!in || !out)throw std::runtime_error("Cannot export motor fault context");}}
}
