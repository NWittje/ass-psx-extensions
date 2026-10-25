#include "coordinator/support_export.hpp"
#include "coordinator/win_handle.hpp"
#include "coordinator/crash_policy.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
namespace coordinator {
namespace d=psx::diagnostics;
namespace b=psx::bridge;
namespace fs=std::filesystem;
namespace {
std::string utf8(const std::wstring& s){if(s.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("UTF8 path");std::string r(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),r.data(),n,nullptr,nullptr);return r;}
std::string quote(std::string_view s){std::ostringstream r;r<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')r<<'\\'<<c;else if(c<32)r<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else r<<c;}r<<'"';return r.str();}
std::string quoted(const std::wstring& s){return quote(utf8(s));}
void save(const fs::path& p,const std::string& text){std::ofstream f(p,std::ios::binary);f.exceptions(std::ios::failbit|std::ios::badbit);f<<text;f.close();}
struct Hash {
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    Hash(){if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider");if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);throw std::runtime_error("SHA256 create");}}
    ~Hash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    void add(std::uint8_t* data,ULONG size){if(BCryptHashData(hash,data,size,0)<0)throw std::runtime_error("SHA256 update");}
    std::string finish(){std::array<unsigned char,32> value{};if(BCryptFinishHash(hash,value.data(),static_cast<ULONG>(value.size()),0)<0)throw std::runtime_error("SHA256 finish");std::ostringstream out;for(auto c:value)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);return out.str();}
};
struct FileResult {std::uint64_t bytes{};DWORD error{};bool changed{};std::string sha;};
FileResult transfer(const fs::path& input,const fs::path& output,std::atomic<bool>& cancelled) {
    FileResult r;WinHandle in(CreateFileW(input.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!in.valid()){r.error=GetLastError();return r;}BY_HANDLE_FILE_INFORMATION before{},after{};if(!GetFileInformationByHandle(in.get(),&before)){r.error=GetLastError();return r;}
    WinHandle out(output.empty()?nullptr:CreateFileW(output.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(!output.empty()&&!out.valid()){r.error=GetLastError();return r;}
    Hash hash;std::array<std::uint8_t,65536> buffer{};
    while(!cancelled){DWORD count=0;if(!ReadFile(in.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr)){r.error=GetLastError();break;}if(!count)break;
        if(out.valid()){DWORD written=0;const auto ok=WriteFile(out.get(),buffer.data(),count,&written,nullptr);const auto error=ok?0:GetLastError();if(!ok||written!=count){r.error=ok?ERROR_WRITE_FAULT:error;break;}}
        hash.add(buffer.data(),count);r.bytes+=count;
    }
    if(cancelled)r.error=ERROR_OPERATION_ABORTED;
    if(!GetFileInformationByHandle(in.get(),&after)){r.error=GetLastError();return r;}
    r.changed=before.nFileSizeLow!=after.nFileSizeLow || before.nFileSizeHigh!=after.nFileSizeHigh || CompareFileTime(&before.ftLastWriteTime,&after.ftLastWriteTime)!=0;
    if(!r.error)r.sha=hash.finish();return r;
}
}
std::string support_snapshot(const ObservationSnapshot& s,const HealthReport& h,const d::JournalStatus& j) {
    const auto& p=s.plugin;std::ostringstream o;o<<std::setprecision(17)<<"{\"qpc_frequency\":"<<qpc_frequency()<<",\"snapshot_qpc\":"<<s.snapshot_qpc<<",\"serialization_qpc\":"<<qpc_now()
        <<",\"source_ready\":"<<h.source_ready<<",\"bridge_connected\":"<<h.bridge_connected<<",\"report_freshness\":"<<unsigned(h.report)<<",\"callback_freshness\":"<<unsigned(h.callback)
        <<",\"plugin_received_qpc\":"<<s.plugin_received_qpc<<",\"plugin_generated_qpc\":"<<p.generated_qpc<<",\"callback_qpc\":"<<p.callback_qpc<<",\"callback_count\":"<<p.callback_count
        <<",\"plugin_load_id\":"<<p.plugin_load_id<<",\"enable_id\":"<<p.enable_id<<",\"counter_baseline_valid\":"<<h.counter_baseline_valid<<",\"delta_valid\":"<<h.counter_delta_valid
        <<",\"diagnostics_dropped_total\":"<<p.diagnostics_dropped<<",\"diagnostics_dropped_since_baseline\":"<<h.diagnostic_dropped<<",\"events_lost\":"<<p.events_lost<<",\"persistence_lost\":"<<p.persistence_lost
        <<",\"journal_run\":"<<j.run_id<<",\"journal_lost\":"<<j.lost<<",\"journal_persistence_lost\":"<<j.persistence_lost<<",\"journal_last_event_error\":"<<j.last_error<<",\"journal_persistence_error\":"<<j.persistence_error
        <<",\"capture_id\":"<<s.capture.capture_id<<",\"capture_stop_reason\":"<<quote(capture_stop_reason(s.capture.stop_reason))<<",\"capture_complete\":"<<s.capture.complete<<",\"cutoff_acknowledged\":"<<s.capture.cutoff.acknowledged<<",\"cutoff_qpc\":"<<s.capture.cutoff.owner_qpc
        <<",\"cutoff_ingestion_sequence\":"<<s.capture.cutoff.ingestion_sequence<<",\"cutoff_motion_epoch\":"<<s.capture.cutoff.motion_epoch<<",\"cutoff_motion_floor\":"<<s.capture.cutoff.motion_sequence_floor<<",\"cutoff_motion_boundary\":"<<unsigned(s.capture.cutoff.motion_boundary)<<",\"footer_qpc\":"<<s.capture.finalized_qpc<<",\"cutoff_sources\":[";
    for(std::size_t i=0;i<s.capture.cutoff.sources.size();++i){const auto& source=s.capture.cutoff.sources[i];if(i)o<<',';o<<'['<<source.sequence<<','<<source.session_epoch<<','<<source.configuration_epoch<<','<<source.boost_generation<<']';}
    o<<"],\"rolling_timing\":{\"decoder_window_limit\":512,\"plugin_window_limit\":128";
    const auto distribution=[&](const char* name,const psx::bridge::Distribution& v){o<<",\""<<name<<"\":{\"n\":"<<v.count<<",\"min\":"<<v.minimum<<",\"p50\":"<<v.p50<<",\"p95\":"<<v.p95<<",\"p99\":"<<v.p99<<",\"max\":"<<v.maximum<<'}';};
    distribution("decode",s.decode_duration);distribution("receive_to_callback",p.receive_to_callback);distribution("duration",p.callback_duration);distribution("interval",p.frame_interval);o<<"},\"enable_lifetime_maxima\":{";
    const auto maximum=[&](const char* name,const d::Maximum& v){o<<'"'<<name<<"\":{\"count\":"<<v.count<<",\"ticks\":"<<v.ticks<<",\"callback\":"<<v.callback_count<<",\"qpc\":"<<v.qpc<<",\"previous_qpc\":"<<v.previous_qpc<<'}';};
    maximum("receive_to_callback",p.receive_to_callback_max);o<<',';maximum("duration",p.callback_duration_max);o<<',';maximum("interval",p.frame_interval_max);o<<'}';
    const auto& source=s.latest.session();
    const auto& motion=s.motion;const auto& request=motion.last_request;const auto& ack=motion.last_ack;const auto& status=motion.status;const auto& pair=status.presentation.source;const auto& geo=status.presentation.geo;
    o<<",\"motion\":{\"connected\":"<<motion.connected<<",\"identity_available\":"<<motion.identity_available<<",\"connection_id\":"<<motion.connection_id<<",\"outstanding\":"<<motion.outstanding<<",\"queued\":"<<motion.queued<<",\"cancelled\":"<<motion.cancelled<<",\"late_acks\":"<<motion.late_acks<<",\"control_overflows\":"<<motion.control_overflows<<",\"transport_fault\":"<<motion.transport_fault
        <<",\"source_motion_epoch\":"<<source.motion_epoch<<",\"source_motion_floor\":"<<source.motion_sequence_floor<<",\"source_boundary\":"<<unsigned(source.motion_boundary)
        <<",\"last_request\":{\"connection\":"<<request.connection_id<<",\"operation\":"<<request.operation_id<<",\"load\":"<<request.plugin_load_id<<",\"enable\":"<<request.enable_id<<",\"aircraft\":"<<request.aircraft_generation<<",\"session\":"<<request.session_epoch<<",\"motion_epoch\":"<<request.motion_epoch<<",\"requested_qpc\":"<<request.requested_qpc<<",\"action\":"<<unsigned(request.action)<<'}'
        <<",\"last_ack\":{\"connection\":"<<ack.connection_id<<",\"operation\":"<<ack.operation_id<<",\"load\":"<<ack.plugin_load_id<<",\"enable\":"<<ack.enable_id<<",\"aircraft\":"<<ack.aircraft_generation<<",\"generated_qpc\":"<<ack.generated_qpc<<",\"result\":"<<unsigned(ack.result)<<",\"reason\":"<<unsigned(ack.reason)<<",\"local_cancellation\":"<<motion.last_ack_local<<'}'
        <<",\"status_available\":"<<motion.status_available<<",\"status_received_qpc\":"<<motion.status_received_qpc<<",\"status\":{\"connection\":"<<status.connection_id<<",\"load\":"<<status.plugin_load_id<<",\"enable\":"<<status.enable_id<<",\"aircraft\":"<<status.aircraft_generation<<",\"scenery\":"<<status.scenery_generation<<",\"revision\":"<<status.revision<<",\"operation\":"<<status.operation_id<<",\"applied_count\":"<<status.applied_count<<",\"discontinuity\":"<<status.discontinuity_generation<<",\"generated_qpc\":"<<status.generated_qpc<<",\"applied_qpc\":"<<status.applied_qpc
        <<",\"phase\":"<<quote(motion_phase_name(status.phase))<<",\"reason\":"<<quote(motion_reason_name(status.reason))<<",\"flags\":"<<status.flags<<",\"reference_revision\":"<<status.reference_revision<<",\"control_dropped\":"<<status.control_dropped<<",\"trace_dropped\":"<<status.trace_dropped
        <<",\"pair\":{\"session\":"<<pair.session_epoch<<",\"boost_generation\":"<<pair.boost_generation<<",\"motion_epoch\":"<<pair.motion_epoch<<",\"boost_sequence\":"<<pair.boost_sequence<<",\"pitch_sequence\":"<<pair.pitch_sequence<<",\"boost_received_qpc\":"<<pair.boost_received_qpc<<",\"pitch_received_qpc\":"<<pair.pitch_received_qpc<<'}'
        <<",\"presentation_lat_lon_msl_m_heading_pitch_bank_deg\":["<<geo.latitude_deg<<','<<geo.longitude_deg<<','<<geo.altitude_msl_m<<','<<geo.heading_deg<<','<<geo.pitch_deg<<','<<geo.bank_deg<<"],\"on_ground\":"<<status.presentation.on_ground;
    const auto local=[&](const char* name,const psx::motion::LocalPose& pose){o<<",\""<<name<<"\":{\"origin_xyz_m\":["<<pose.origin.x<<','<<pose.origin.y<<','<<pose.origin.z<<"],\"heading_pitch_bank_deg\":["<<pose.heading_deg<<','<<pose.pitch_deg<<','<<pose.bank_deg<<"]}";};
    local("written",status.written);local("readback",status.readback);o<<",\"position_error_m\":"<<status.position_error_m<<",\"attitude_error_deg\":"<<status.attitude_error_deg;
    distribution("pre_callback",status.pre_callback_duration);distribution("post_callback",status.post_callback_duration);distribution("boost_receive_to_apply",status.boost_receive_to_apply);distribution("pitch_age",status.pitch_age);distribution("frame_interval",status.frame_interval);distribution("output_age",status.output_age);distribution("hold_duration",status.hold_duration);
    const auto& trace=status.reconstruction_trace;
    o<<",\"reconstruction\":{\"mode\":"<<unsigned(status.reconstruction.mode)<<",\"delay_ms\":"<<status.reconstruction.delay_ms<<",\"state\":"<<unsigned(trace.state)<<",\"window_revision\":"<<trace.window_revision<<",\"left_sequence\":"<<trace.left_sequence<<",\"right_sequence\":"<<trace.right_sequence<<",\"target_qpc\":"<<trace.target_qpc<<",\"presentation_qpc\":"<<trace.presentation_qpc<<",\"left_received_qpc\":"<<trace.left_received_qpc<<",\"right_received_qpc\":"<<trace.right_received_qpc<<",\"alpha\":"<<trace.alpha<<",\"boost_count\":"<<status.boost_history_count<<",\"pitch_count\":"<<status.pitch_history_count
        <<",\"bracketed_frames\":"<<status.bracketed_count<<",\"oldest_hold_frames\":"<<status.oldest_hold_count<<",\"newest_hold_frames\":"<<status.newest_hold_count<<",\"no_pitch_frames\":"<<status.no_pitch_count<<",\"history_resets\":"<<status.history_reset_count<<",\"hold_episodes\":"<<status.hold_transitions<<"}}}";
    const auto& window=s.motion_window;const auto& counters=s.history_counters;
    const auto& selection=s.selected_window;
    const auto& exporter=s.exporter;const auto& full=s.complete_window;const auto& applied=status.presentation.provenance;
    const auto& av=s.audio;
    o<<",\"audio\":{\"epoch\":"<<av.state.audio_epoch<<",\"following\":"<<av.following<<",\"muted\":"<<av.muted<<",\"available\":"<<av.state.available_groups<<",\"suppression_capabilities\":"<<av.state.suppression_capabilities<<",\"acknowledged_groups\":"<<av.ownership.accepted_groups<<",\"lease_operation\":"<<av.ownership.lease.operation<<",\"lease_expiry_qpc\":"<<av.ownership.lease.expires_qpc
        <<",\"ready\":"<<av.plugin.ready_groups<<",\"active\":"<<av.plugin.active_groups<<",\"held\":"<<av.plugin.held_groups<<",\"missing\":"<<av.plugin.missing_groups<<",\"plugin_enable_id\":"<<av.plugin.plugin_generation<<",\"aircraft_generation\":"<<av.plugin.aircraft_generation<<",\"report_received_qpc\":"<<av.status_received<<",\"applied_sequence\":"<<av.plugin.applied_sequence<<",\"applied_sample_qpc\":"<<av.plugin.applied_sample_qpc
        <<",\"received\":"<<av.received<<",\"rejected\":"<<av.rejected<<",\"transitions\":"<<av.transitions<<",\"overflows\":"<<av.overflows<<",\"late_acks\":"<<av.late_acks<<",\"groups\":[";
    bool first_audio=true;for(std::size_t i=0;i<psx::audio::group_count;++i)if(av.state.available_groups&(1u<<i)){if(!first_audio)o<<',';first_audio=false;const auto& g=av.state.groups[i];o<<"{\"group\":"<<i<<",\"family\":"<<unsigned(g.family)<<",\"variant\":"<<unsigned(g.variant)<<",\"sequence\":"<<g.sequence<<",\"lifecycle_epoch\":"<<g.lifecycle_epoch<<",\"configuration_epoch\":"<<g.configuration_epoch<<",\"sample_start_qpc\":"<<g.sample_start_qpc<<",\"sample_end_qpc\":"<<g.sample_end_qpc<<",\"received_qpc\":"<<g.received_qpc<<",\"flags\":"<<g.flags<<",\"valid_fields\":"<<g.valid_fields<<",\"raw_values\":[";for(std::size_t f=0;f<g.values.size();++f){if(f)o<<',';o<<g.values[f];}o<<"]}";}o<<"]}";
    o<<",\"exporter\":{\"pid\":"<<exporter.process.pid<<",\"creation_filetime\":"<<exporter.process.creation_filetime<<",\"peer\":"<<exporter.peer<<",\"ready\":"<<exporter.ready<<",\"error\":"<<exporter.error
        <<",\"producer\":["<<exporter.hello.producer.high<<','<<exporter.hello.producer.low<<"],\"schema\":"<<exporter.hello.schema_revision<<",\"build\":"<<quote(exporter.hello.build.data())<<",\"qpc_frequency\":"<<exporter.hello.qpc_frequency
        <<",\"raw\":"<<exporter.raw<<",\"invalid\":"<<exporter.invalid<<",\"sequence_gaps\":"<<exporter.sequence_gaps<<",\"frame_gaps\":"<<exporter.frame_gaps<<",\"queue_dropped\":"<<exporter.health.queue_dropped<<",\"transport_dropped\":"<<exporter.health.transport_dropped<<",\"handoff_dropped\":"<<exporter.handoff_dropped
        <<",\"stable_since\":"<<exporter.stable_since<<",\"last_valid_receipt_qpc\":"<<exporter.last_valid_qpc<<",\"last_sample_end_qpc\":"<<exporter.last_sample_end_qpc<<",\"sampling_width_qpc\":"<<exporter.last_sampling_width
        <<",\"selected_source\":"<<unsigned(exporter.selection.source)<<",\"selection_epoch\":"<<exporter.selection.selection_epoch<<",\"reason\":"<<quote(selection_reason_name(exporter.selection.reason))<<",\"source_preference\":"<<unsigned(exporter.preference)
        <<",\"history_count\":"<<full.count<<",\"history_revision\":"<<full.revision<<",\"sequence_floor\":"<<full.sequence_floor<<",\"pose_epoch\":"<<full.domain.pose_epoch<<",\"reference_revision\":"<<full.domain.reference_revision
        <<",\"oldest_sample_qpc\":"<<(full.count?full.samples[0].sample_end_qpc:0)<<",\"newest_sample_qpc\":"<<(full.count?full.samples[full.count-1].sample_end_qpc:0)<<",\"accepted\":"<<s.complete_counters.accepted<<",\"evicted\":"<<s.complete_counters.evicted<<",\"boundaries\":"<<exporter.boundaries<<",\"windows_selected_to_pipe\":"<<s.complete_windows_sent<<",\"windows_superseded\":"<<s.complete_windows_superseded<<'}';
    o<<",\"source_provenance\":{\"plugin_selected\":"<<unsigned(status.selected_source)<<",\"plugin_selection_epoch\":"<<status.selection_epoch<<",\"applied_source\":"<<unsigned(applied.kind)<<",\"applied_selection_epoch\":"<<applied.selection_epoch
        <<",\"producer\":["<<applied.domain.producer.high<<','<<applied.domain.producer.low<<"],\"pose_epoch\":"<<applied.domain.pose_epoch<<",\"reference_revision\":"<<applied.domain.reference_revision<<",\"left_sequence\":"<<applied.left_sequence<<",\"right_sequence\":"<<applied.right_sequence<<",\"frame_sequence\":"<<applied.frame_sequence
        <<",\"sample_start_qpc\":"<<applied.sample_start_qpc<<",\"sample_end_qpc\":"<<applied.sample_end_qpc<<",\"receipt_qpc\":"<<applied.received_qpc<<",\"handoff_position_m\":"<<status.handoff_position_m<<",\"handoff_attitude_deg\":"<<status.handoff_attitude_deg
        <<",\"request_source_preference\":"<<unsigned(request.source_preference)<<",\"request_selection_epoch\":"<<request.expected_selection_epoch;distribution("sample_to_apply",status.exporter_sample_to_apply);distribution("sampling_width",status.sampling_width);o<<'}';
    o<<",\"last_window_selected_to_pipe\":{\"available\":"<<selection.available<<",\"connection_id\":"<<selection.connection_id<<",\"session_revision\":"<<selection.session_revision<<",\"window_revision\":"<<selection.window_revision<<",\"boost_count\":"<<selection.boost_count<<",\"first_sequence\":"<<selection.first_sequence<<",\"last_sequence\":"<<selection.last_sequence<<",\"first_received_qpc\":"<<selection.first_received_qpc<<",\"last_received_qpc\":"<<selection.last_received_qpc<<",\"published_qpc\":"<<selection.published_qpc<<'}';
    o<<",\"motion_history\":{\"revision\":"<<window.revision<<",\"boost_count\":"<<window.boost_count<<",\"pitch_count\":"<<window.pitch_count<<",\"pitch_anchor\":"<<window.has_pitch_anchor<<",\"oldest_boost_qpc\":"<<(window.boost_count?window.boost[0].stamp.received_qpc:0)<<",\"newest_boost_qpc\":"<<(window.boost_count?window.boost[window.boost_count-1].stamp.received_qpc:0)
        <<",\"decoded_boost\":"<<counters.decoded_boost<<",\"decoded_pitch\":"<<counters.decoded_pitch<<",\"evicted_boost\":"<<counters.evicted_boost<<",\"evicted_pitch\":"<<counters.evicted_pitch<<",\"equal_time_boost\":"<<counters.equal_time_boost<<",\"rejected\":"<<counters.rejected<<",\"resets\":"<<counters.resets<<",\"windows_published\":"<<s.motion_windows_published<<",\"windows_selected_to_pipe\":"<<s.motion_windows_sent<<",\"windows_superseded\":"<<s.motion_windows_superseded<<",\"request_mode\":"<<unsigned(request.reconstruction.mode)<<",\"request_delay_ms\":"<<request.reconstruction.delay_ms<<'}';
    o<<",\"psx\":{\"version\":"<<quote(source.psx_version.data())<<",\"family\":"<<static_cast<unsigned>(source.family)<<",\"flags\":"<<source.flags<<",\"session_epoch\":"<<source.session_epoch<<",\"configuration_epoch\":"<<source.configuration_epoch<<",\"boost_generation\":"<<source.boost_generation<<",\"revision\":"<<source.revision<<",\"published_qpc\":"<<source.published_qpc<<'}';
    const auto channel=[&](const char* name,const ChannelCounters& c){o<<",\""<<name<<"\":{\"state\":"<<quote(state_name(c.state))<<",\"attempts\":"<<c.attempts<<",\"bytes_in\":"<<c.bytes_in<<",\"bytes_out\":"<<c.bytes_out<<",\"lines\":"<<c.lines<<",\"errors\":"<<c.errors<<",\"truncated\":"<<c.truncated<<",\"retry_seconds\":"<<c.retry_seconds<<",\"last_error\":"<<c.last_error<<'}';};
    channel("main",s.main);channel("boost",s.boost);
    o<<",\"bridge\":{\"state\":"<<quote(state_name(s.bridge))<<",\"last_error\":"<<s.bridge_last_error<<",\"peer_process_id\":"<<s.bridge_peer_process_id<<",\"last_report_connection_id\":"<<p.connection_id<<",\"attachments\":"<<s.bridge_attachments<<",\"rejected\":"<<s.bridge_rejected<<",\"source_coalesced\":"<<s.bridge_coalesced<<",\"control_overflows\":"<<s.bridge_control_overflows<<",\"write_stalls\":"<<s.bridge_write_stalls<<",\"diagnostic_handoff_lost\":"<<s.diagnostic_handoff_lost<<'}';
    o<<",\"decoder\":{\"selected\":"<<s.selected<<",\"malformed\":"<<s.malformed<<",\"unsupported\":"<<s.unsupported<<",\"bootstrap_errors\":"<<s.bootstrap_errors<<'}';
    o<<",\"aircraft\":{\"identity\":"<<quote(p.aircraft.data())<<",\"aircraft_generation\":"<<p.aircraft_generation<<",\"scenery_generation\":"<<p.scenery_generation<<"},\"sources\":[";
    for(std::size_t i=1;i<b::source_slots;++i){if(i>1)o<<',';const auto& m=s.latest.records()[i];const auto& stamp=m.observation.stamp;o<<"{\"source\":"<<i<<",\"available\":"<<s.latest.available()[i]<<",\"plugin_consumed_sequence\":"<<p.consumed_sequence[i]<<",\"plugin_received_qpc\":"<<p.received_qpc[i];
        if(s.latest.available()[i])o<<",\"sequence\":"<<stamp.sequence<<",\"received_qpc\":"<<stamp.received_qpc<<",\"decoded_qpc\":"<<m.decoded_qpc<<",\"published_qpc\":"<<m.published_qpc<<",\"session_epoch\":"<<stamp.session_epoch<<",\"configuration_epoch\":"<<stamp.configuration_epoch<<",\"boost_generation\":"<<m.boost_generation;
        o<<'}';}o<<"]}\n";return o.str();
}
SupportExporter::~SupportExporter(){cancel();join();}
void SupportExporter::cancel() noexcept{cancelled_=true;if(worker_.joinable())CancelSynchronousIo(worker_.native_handle());}
void SupportExporter::join(){if(worker_.joinable())worker_.join();}
ExportStatus SupportExporter::status() const{std::lock_guard<std::mutex> lock(mutex_);return status_;}
bool SupportExporter::start(ExportRequest request) {
    if(!done_ || !fs::path(request.directory).is_absolute() || request.directory.rfind(L"\\\\",0)==0)return false;join();cancelled_=false;done_=false;
    {std::lock_guard<std::mutex> lock(mutex_);status_={};status_.running=true;status_.directory=request.directory;}
    try{worker_=std::thread([this,request=std::move(request)]{try{run(request);}catch(...){std::lock_guard<std::mutex> lock(mutex_);status_.partial=true;status_.error=ERROR_UNHANDLED_EXCEPTION;} {std::lock_guard<std::mutex> lock(mutex_);status_.running=false;}done_=true;});}
    catch(...){done_=true;std::lock_guard<std::mutex> lock(mutex_);status_.running=false;status_.partial=true;status_.error=ERROR_NOT_ENOUGH_MEMORY;return false;}return true;
}
void SupportExporter::run(const ExportRequest& request) {
    const auto events=hub_.producer(4,d::Component::exporter);events.emit(d::Code::export_start);
    const auto started=qpc_now();const fs::path directory(request.directory);std::error_code ec;
    if(!fs::create_directory(directory,ec)){const auto error=ec?static_cast<DWORD>(ec.value()):ERROR_ALREADY_EXISTS;events.emit(d::Code::export_incomplete,d::Severity::warning,{},d::ErrorDomain::win32,error,"Export directory creation failed");std::lock_guard<std::mutex> lock(mutex_);status_.partial=true;status_.error=error;return;}
    bool complete=true;std::uint64_t count=0;std::ostringstream entries;bool first=true;
    auto seal=hub_.seal();struct Release {d::DiagnosticsHub& hub;std::uint64_t ticket;~Release(){hub.release_seal(ticket);}} release{hub_,seal.ticket};
    complete=seal.complete;
    const auto record=[&](const fs::path& path,bool copy,bool sealed,const char* category,bool generated=false,const ReplayResult* replay=nullptr) {
        const auto name=std::to_wstring(++count)+L"-"+path.filename().wstring();FileResult r;
        try{r=transfer(path,copy?directory/name:fs::path{},cancelled_);}catch(...){r.error=ERROR_UNHANDLED_EXCEPTION;}
        if(r.error || r.changed)complete=false;if(!first)entries<<',';first=false;
        entries<<"{\"source\":"<<quoted(path.wstring())<<",\"category\":"<<quote(category)<<",\"copied\":"<<copy<<",\"generated\":"<<generated<<",\"file\":"<<(copy?quoted(name):generated?quoted(path.filename().wstring()):"null")<<",\"sealed\":"<<sealed<<",\"source_changed\":"<<r.changed<<",\"bytes\":"<<r.bytes<<",\"sha256\":"<<quote(r.sha)<<",\"win32_error\":"<<r.error;
        if(replay)entries<<",\"capture_validation\":{\"valid\":"<<replay->valid<<",\"complete\":"<<replay->complete<<",\"version\":"<<replay->capture_version<<",\"error\":"<<quote(replay->error)<<'}';entries<<'}';
        std::lock_guard<std::mutex> lock(mutex_);status_.files=count;if(r.error)status_.error=r.error;
    };
    // Write a pessimistic manifest immediately so interruption cannot leave a claimed complete export.
    save(directory/L"manifest.json","{\"complete\":false,\"reason\":\"export in progress or interrupted\"}\n");
    for(const auto& path:seal.files)record(path,true,true,"sealed coordinator journal (includes received plugin events, not full plugin history)");
    // A foreign process has no remote seal service. Only recognized closed
    // segments are pinned, denying writes/deletes for the duration of each copy.
    const auto plugin_directory=fs::path(hub_.options().directory).parent_path()/L"Plugin";
    const auto skipped=[&](const fs::path& file,const char* reason,DWORD error=0){complete=false;if(!first)entries<<',';first=false;entries<<"{\"source\":"<<quoted(file.wstring())<<",\"copied\":false,\"skip_reason\":"<<quote(reason)<<",\"win32_error\":"<<error<<'}';};
    std::error_code plugin_error;
    if(fs::exists(plugin_directory,plugin_error)) {
        fs::directory_iterator it(plugin_directory,plugin_error),end;
        for(;!plugin_error && it!=end;it.increment(plugin_error)) {
            const auto file=it->path();if(file.filename().wstring().rfind(L"psxdiag-",0)!=0 || file.extension()!=L".jsonl")continue;
            const auto attributes=GetFileAttributesW(file.c_str());if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){skipped(file,"plugin segment invalid/reparse path",ERROR_INVALID_DATA);continue;}
            WinHandle pin(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(!pin.valid()){const auto error=GetLastError();skipped(file,"active or unavailable plugin segment; no remote seal service",error);continue;}
            std::array<char,28> signature{};DWORD read=0;
            if(!ReadFile(pin.get(),signature.data(),static_cast<DWORD>(signature.size()),&read,nullptr) || read!=signature.size() || std::string_view(signature.data(),signature.size()).find("{\"kind\":\"psx_diagnostics\"")!=0) {skipped(file,"unrecognized plugin journal header",ERROR_INVALID_DATA);continue;}
            record(file,true,true,"closed plugin journal, pinned read-only; process/crash completeness not inferred");
        }
        if(plugin_error)skipped(plugin_directory,"plugin directory enumeration incomplete",static_cast<DWORD>(plugin_error.value()));
    } else {if(!first)entries<<',';first=false;entries<<"{\"source\":"<<quoted(plugin_directory.wstring())<<",\"copied\":false,\"status\":\"no plugin journal directory available\"}";complete=false;}
    save(directory/L"snapshot.json",support_snapshot(request.observation,request.health,hub_.status()));
    const auto& o=hub_.options();const auto& c=o.config;std::ostringstream config;
    config<<"enabled="<<c.enabled<<"\ncrash_dumps="<<c.crash_dumps<<"\nsegment_bytes="<<c.segment_bytes<<"\nretained_segments="<<c.retained_segments<<"\nflush_ms="<<c.flush_ms<<"\nrepeat_ms="<<c.repeat_ms<<"\nmetrics_ms="<<c.metrics_ms<<"\nstatus_ms="<<c.status_ms<<"\ndelayed_ms="<<c.delayed_ms<<"\ngap_ms="<<c.gap_ms<<"\nvalid="<<c.valid<<"\ndirectory="<<utf8(o.directory)<<"\nconfig_path="<<utf8(o.config_path)<<"\nbuild="<<o.build<<'\n';
    save(directory/L"effective-settings.txt",config.str());
    WindowsCrashRegistry registry;const auto policy=inspect_crash_policy(registry,c.crash_dumps,crash_dump_directory());std::ostringstream wer;
    wer<<"{\"scope\":\"HKLM 64-bit LocalDumps/psx-xp-coordinator.exe only; inherited/global policies not evaluated\",\"desired\":"<<policy.desired<<",\"key_exists\":"<<policy.key_exists<<",\"matching_mini_policy\":"<<policy.effective<<",\"owned\":"<<policy.owned<<",\"conflict\":"<<policy.conflict<<",\"needs_setup\":"<<policy.needs_setup<<",\"win32_error\":"<<policy.error<<",\"type_present\":"<<policy.type_present<<",\"actual_type\":"<<policy.actual_type<<",\"count_present\":"<<policy.count_present<<",\"actual_count\":"<<policy.actual_count<<",\"actual_folder\":"<<quoted(policy.folder)<<"}\n";save(directory/L"wer-policy.json",wer.str());
    std::array<wchar_t,32768> executable{};const auto n=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(n && n<executable.size()){const fs::path exe(executable.data());record(exe,false,false,"running executable identity only");auto pdb=exe;pdb.replace_extension(L".pdb");record(pdb,false,false,"matching-name symbols identity only; signature pairing not inferred");}else complete=false;
    const std::set<std::wstring> allowed{L".log",L".txt",L".json",L".jsonl",L".rpt",L".dmp",L".psxcap",L".psxobs"};
    for(const auto& path:request.selected_files){const fs::path file(path);auto extension=file.extension().wstring();for(auto& ch:extension)ch=static_cast<wchar_t>(towlower(ch));
        if(!file.is_absolute() || allowed.find(extension)==allowed.end()){skipped(file,"unsupported selected file path or extension");continue;}
        if(extension==L".psxcap" || extension==L".psxobs"){const auto replay=replay_capture(path);if(!replay.valid || !replay.complete)complete=false;record(file,true,false,"operator-selected capture; validation does not discard failure evidence",false,&replay);continue;}
        record(file,true,false,"operator-selected existing file; copied at export time");
    }
    for(const auto* name:{L"snapshot.json",L"effective-settings.txt",L"wer-policy.json"})record(directory/name,false,false,"generated support artifact",true);
    if(cancelled_)complete=false;
    std::ostringstream manifest;manifest<<"{\"schema\":1,\"complete\":"<<(complete?"true":"false")<<",\"seal_complete\":"<<(seal.complete?"true":"false")<<",\"started_qpc\":"<<started<<",\"ended_qpc\":"<<qpc_now()<<",\"files\":["<<entries.str()<<"],\"limits\":\"Local folder only. Existing external files are not sealed. Snapshot, capture cutoff, footer and shutdown have distinct times. No binaries copied or upload.\"}\n";
    save(directory/L"manifest.json",manifest.str());
    {std::lock_guard<std::mutex> lock(mutex_);status_.complete=complete;status_.partial=!complete;}
    events.emit(complete?d::Code::export_complete:d::Code::export_incomplete,complete?d::Severity::info:d::Severity::warning);
}
}

