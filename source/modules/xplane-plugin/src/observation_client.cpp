#include <psx/sync/worker_io.hpp>
#include "psx_plugin/observation_client.hpp"
#include "psx_plugin/triple_buffer.hpp"
#include "psx_plugin/scenery_coverage.hpp"
#include "psx_plugin/motion_window.hpp"
#include "psx_plugin/complete_window.hpp"
#include "psx_plugin/audio_model.hpp"
#include <psx/non_normal/pipe.hpp>
#include <Windows.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <thread>

namespace psx::xp {
namespace d = diagnostics;
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE v) noexcept : value(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
constexpr std::size_t window_size = 128;
struct Samples {
    std::array<std::uint64_t,window_size> ticks{};
    std::size_t count{}, next{};
    void add(std::uint64_t value) noexcept {
        ticks[next] = value; next = (next + 1) % window_size;
        count = (std::min)(count + 1, window_size);
    }
    bridge::Distribution distribution() const noexcept {
        if (!count) return {};
        auto sorted = ticks;
        std::sort(sorted.begin(), sorted.begin() + count);
        return {count,sorted[0],sorted[count-1],sorted[(count*50+99)/100-1],
            sorted[(count*95+99)/100-1],sorted[(count*99+99)/100-1]};
    }
};
struct CompleteTiming {
    Samples samples{};
    std::uint64_t connection{},selection{},lifecycle{};
    motion::PoseDomain domain{};
    void scope(const bridge::CompletePoseWindow& window,std::uint64_t generation) noexcept {
        if(connection!=window.connection_id||selection!=window.selection_epoch||domain!=window.samples.domain||lifecycle!=generation)samples={};
        connection=window.connection_id;selection=window.selection_epoch;domain=window.samples.domain;lifecycle=generation;
    }
};
struct Diagnostics {
    bridge::PluginStatus status{};
    Samples age{}, duration{}, interval{};
    CompleteTiming complete_consumption{};
    std::uint64_t lifecycle_generation{};
};
// Local handoff metadata only. The wire window and all producer timestamps stay unchanged.
struct CompleteHandoff {
    bridge::CompletePoseWindow window{};
    std::int64_t received_qpc{};
    std::uint64_t lifecycle_generation{};
};
struct Snapshot {
    bridge::LatestState latest{};
    bool connected{},authenticated{}; // OS provenance belongs to this attachment, not current network liveness.
    non_normal::ProcessIdentity peer{};
    std::uint64_t rejected{}, coalesced{};
};
std::array<std::uint64_t,6> maximum_values(const d::Maximum& value,std::uint64_t kind) noexcept {
    return {value.count,value.ticks,value.callback_count,static_cast<std::uint64_t>(value.qpc),
        static_cast<std::uint64_t>(value.previous_qpc),kind};
}
std::uint64_t identity() noexcept {
    // Seed once off the SDK thread; the increment ensures different attachments
    // in this process cannot collide just because their QPC values differ.
    static std::atomic<std::uint64_t> identities{static_cast<std::uint64_t>(observation_qpc()) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32)};
    auto value=identities.fetch_add(1);
    if (!value) value=identities.fetch_add(1);
    return value;
}
}
std::int64_t observation_qpc() noexcept {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart;
}
std::uint64_t observation_qpc_frequency() noexcept {
    LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<std::uint64_t>(value.QuadPart);
}
std::wstring observation_endpoint() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)) return {};
    // TOKEN_USER plus the largest SID fits this aligned bounded buffer.
    alignas(TOKEN_USER) std::array<std::byte,sizeof(TOKEN_USER)+SECURITY_MAX_SID_SIZE> bytes{};
    DWORD size{};
    if (!GetTokenInformation(token.value,TokenUser,bytes.data(),static_cast<DWORD>(bytes.size()),&size)) return {};
    LPWSTR sid{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)) return {};
    struct LocalString { LPWSTR p; ~LocalString() { LocalFree(p); } } owned{sid};
    return std::wstring(L"\\\\.\\pipe\\psx-ios.observation.v1.") + sid;
}
struct ObservationClient::Impl {
    std::wstring endpoint;
    Handle stop_event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::thread worker;
    TripleBuffer<Snapshot> incoming;
    TripleBuffer<bridge::MotionWindow> windows;
    MotionWindowGate window_gate; // Worker only; SDK consumes admitted complete windows.
    TripleBuffer<CompleteHandoff> complete_windows;
    CompleteTiming complete_receipt{}; // Worker owned: every accepted complete window.
    std::atomic<std::uint64_t> timing_lifecycle{}; // SDK invalidation -> worker receipt metadata.
    CompleteWindowGate complete_gate;
    SourceSelectionGate selection_gate;
    TripleBuffer<audio::State> audio_states;
    AudioStateGate audio_gate;
    SpscFifo<audio::Control,64> audio_controls;
    SpscFifo<audio::Transition,audio::transition_capacity> audio_transitions;
    TripleBuffer<audio::Status> audio_outgoing;
    audio::Status audio_diagnostic{};
    std::atomic<std::uint64_t> audio_fault_generation{};
    std::uint64_t audio_fault_seen{},audio_blocked_epoch{},audio_transition_sequence{};
    bool audio_blocked{};
    std::int64_t last_audio{};
    TripleBuffer<Diagnostics> outgoing;
    SpscFifo<MotionControl,32> motion_controls;
    SpscFifo<bridge::MotionAck,64> motion_acks;
    std::unique_ptr<SceneryCoverage> coverage;
    std::thread coverage_thread;
    SpscFifo<bridge::CoverageRequest,16> coverage_requests;
    SpscFifo<bridge::CoverageReply,16> coverage_replies;
    void coverage_run() noexcept {
        while(!stopped()) {
            bridge::CoverageRequest request;
            if(!coverage_requests.pop(request)){WaitForSingleObject(stop_event.value,10);continue;}
            if(request.connection_id!=current_connection.load(std::memory_order_acquire))continue;
            bridge::CoverageReply reply;reply.request=request;reply.result.query=request.query;
            if(request.plugin_load_id!=load_id||request.enable_id!=enable_id)reply.result.reason=scenery::Reason::identity;
            else if(observation_qpc()>=request.deadline_qpc)reply.result.reason=scenery::Reason::timeout;
            else reply.result=coverage->inspect(request.query);
            const auto now=observation_qpc();
            if(now>=request.deadline_qpc){reply.result.verdict=scenery::Verdict::unavailable;reply.result.reason=scenery::Reason::timeout;}
            reply.result.plugin_load=load_id;reply.result.plugin_enable=enable_id;
            reply.result.observed_qpc=reply.result.source_qpc=now;reply.result.source_frequency=frequency;
            if(request.connection_id==current_connection.load(std::memory_order_acquire))coverage_replies.push(reply);
        }
    }
    bool prepare_coverage_reply(std::uint32_t& sequence,bridge::Frame& output) noexcept {
        bridge::CoverageReply reply;
        for(unsigned i=0;i<16&&coverage_replies.pop(reply);++i)
            if(reply.request.connection_id==producer.latest.session().connection_id)return bridge::encode(reply,++sequence,output);
        return false;
    }
    ~Impl(){SetEvent(stop_event.value);if(worker.joinable())worker.join();if(coverage_thread.joinable())coverage_thread.join();}
    SpscFifo<bridge::ViewRequest,32> view_requests;
    SpscFifo<bridge::ViewReply,64> view_replies;
    std::atomic<std::uint64_t> view_replies_dropped{};
    TripleBuffer<MotionReport> motion_outgoing;
    TripleBuffer<ProfileRequest> profile_requests;
    TripleBuffer<MotionProfile> profiles;
    std::atomic<bool> motion_fault{};
    std::atomic<std::uint64_t> motion_fault_generation{};
    std::uint64_t motion_fault_seen{},motion_trace_dropped{}; // SDK owner only.
    std::atomic<std::uint64_t> current_connection{};
    MotionReport motion_diagnostic{}; // Worker only.
    std::int64_t last_motion{};
    Snapshot producer{}; // Worker only.
    Diagnostics consumer{}; // Callback only.
    bridge::Framer framer;
    std::uint64_t frequency{observation_qpc_frequency()};
    std::int64_t last_peer{};
    bool handshake{};
    bool lifecycle_invalidated{}; // Callback owner only.
    d::DiagnosticsHub* journal{};
    d::DiagnosticProducer sdk_events, worker_events;
    d::Config config{};
    std::uint64_t load_id{}, enable_id{}, recent_cursor{};
    std::int64_t (*callback_clock)() noexcept{observation_qpc};
    std::int64_t (*receipt_clock)() noexcept{observation_qpc};
    Diagnostics diagnostic{}; // Worker only, survives pipe reconnect.
    bool callback_delayed{};
    std::int64_t last_metrics{}, last_events{};
    d::JournalStatus journal_status{};
    void fail_motion_transport() noexcept {motion_fault_generation.fetch_add(1,std::memory_order_relaxed);motion_fault.store(true,std::memory_order_release);}

    explicit Impl(std::wstring name,const ObservationOptions& options) : endpoint(std::move(name)),journal(options.diagnostics),
        load_id(options.plugin_load_id ? options.plugin_load_id : identity()),
        enable_id(options.enable_id ? options.enable_id : 1) {
        try{if(!options.scenery_root.empty())coverage=std::make_unique<SceneryCoverage>(options.scenery_root);}catch(...){coverage.reset();}
        if(options.callback_clock) callback_clock=options.callback_clock;
        if(options.receipt_clock) receipt_clock=options.receipt_clock;
        if(journal) { sdk_events=journal->producer(0,d::Component::plugin); worker_events=journal->producer(1,d::Component::bridge); config=journal->options().config; }
        consumer.status.plugin_load_id=load_id; consumer.status.enable_id=enable_id;
        consumer.status.diagnostic_validity=bridge::counter_baseline_valid;
        diagnostic=consumer;
    }
    d::Context context(const bridge::PluginStatus& s) const noexcept {
        d::Context c; c.plugin_load_id=load_id; c.enable_id=enable_id;
        c.connection_id=s.connection_id; c.session_epoch=s.session_epoch; c.configuration_epoch=s.configuration_epoch;
        c.boost_generation=s.boost_generation; c.aircraft_generation=s.aircraft_generation; c.scenery_generation=s.scenery_generation; return c;
    }
    void event(d::Code code,d::Severity severity=d::Severity::info,d::ErrorDomain domain=d::ErrorDomain::none,
               std::uint32_t error=0,std::string_view message={},std::array<std::uint64_t,6> values={}) noexcept {
        auto c=context(diagnostic.status); const auto& session=producer.latest.session();
        c.connection_id=session.connection_id; c.session_epoch=session.session_epoch;
        c.configuration_epoch=session.configuration_epoch; c.boost_generation=session.boost_generation;
        worker_events.emit(code,severity,c,domain,error,message,values);
    }
    bool elapsed(std::int64_t now,std::int64_t then,std::uint32_t ms) const noexcept {
        return now>=then && static_cast<std::uint64_t>(now-then)>=frequency*ms/1000;
    }
    void complete_metric(const CompleteTiming& timing,const char* stage,bool final) noexcept {
        if(timing.lifecycle!=diagnostic.lifecycle_generation)return; // Await matching SDK lifecycle identity.
        const auto& selected=selection_gate.value();
        if(!final&&(timing.connection!=current_connection.load(std::memory_order_acquire)||timing.selection!=selected.selection_epoch||timing.domain!=selected.domain))return;
        const auto value=timing.samples.distribution();
        std::array<char,160> label{};
        std::snprintf(label.data(),label.size(),"complete-window %s%s exporter s=%llu p=%016llX:%016llX e=%llu r=%u",stage,final?" final":"",
            static_cast<unsigned long long>(timing.selection),static_cast<unsigned long long>(timing.domain.producer.high),
            static_cast<unsigned long long>(timing.domain.producer.low),static_cast<unsigned long long>(timing.domain.pose_epoch),timing.domain.reference_revision);
        auto c=context(diagnostic.status);c.connection_id=timing.connection;
        worker_events.emit(d::Code::metrics,d::Severity::info,c,d::ErrorDomain::none,0,label.data(),
            {value.count,value.p50,value.p95,value.p99,value.maximum,frequency});
    }
    void complete_metrics(bool final=false) noexcept { // Worker only; no sorting/formatting on the SDK thread.
        complete_metric(complete_receipt,"publish-to-receipt",final);
        complete_metric(diagnostic.complete_consumption,"receipt-to-consumption",final);
    }
    void consume_complete() noexcept { // SDK owner: first ownership transfer, not pose application.
        if(!complete_windows.consume())return;
        const auto& value=complete_windows.reading();
        if(!value.window.connection_id||value.lifecycle_generation!=consumer.lifecycle_generation){consumer.complete_consumption={};return;}
        consumer.complete_consumption.scope(value.window,consumer.lifecycle_generation);
        if(value.received_qpc<=0)return;
        const auto consumed=callback_clock();
        if(consumed>=value.received_qpc)consumer.complete_consumption.samples.add(static_cast<std::uint64_t>(consumed-value.received_qpc));
    }
    void service_diagnostics(std::int64_t now) noexcept {
        if(profile_requests.consume()) { profiles.writing()=verify_motion_profile(profile_requests.reading());profiles.publish(); }
        if(motion_outgoing.consume())motion_diagnostic=motion_outgoing.reading();
        if(outgoing.consume()) diagnostic=outgoing.reading();
        if(complete_receipt.lifecycle!=timing_lifecycle.load(std::memory_order_acquire))complete_receipt={};
        const auto& status=diagnostic.status;
        const bool delayed=d::freshness(status.callback_qpc>0,now,status.callback_qpc,frequency,config.delayed_ms)==d::Freshness::delayed;
        if(delayed!=callback_delayed) {
            event(delayed ? d::Code::callback_delayed : d::Code::callback_resumed,d::Severity::info,
                d::ErrorDomain::none,0,{}, {status.callback_count,static_cast<std::uint64_t>(status.callback_qpc),static_cast<std::uint64_t>(now),0,0,0});
            callback_delayed=delayed;
        }
        if(journal && elapsed(now,last_metrics,config.metrics_ms)) {
            journal_status=journal->status();
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,{},
                {status.callback_count,status.callback_duration_max.ticks,status.frame_interval_max.ticks,
                 status.diagnostics_dropped,producer.coalesced,journal_status.lost});
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"receive maximum",maximum_values(status.receive_to_callback_max,0));
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"duration maximum",maximum_values(status.callback_duration_max,1));
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"interval maximum",maximum_values(status.frame_interval_max,2));
            const auto& motion=motion_diagnostic.status;
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"reconstruction frames: bracket/oldest/newest/no-pitch/reset/episodes",
                {motion.bracketed_count,motion.oldest_hold_count,motion.newest_hold_count,motion.no_pitch_count,motion.history_reset_count,motion.hold_transitions});
            const auto age=motion_diagnostic.output_age.distribution(),hold=motion_diagnostic.hold_duration.distribution();
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"reconstruction QPC ticks: output-age count/p50/p99; hold-duration count/p50/p99",
                {age.count,age.p50,age.p99,hold.count,hold.p50,hold.p99});
            const auto sample_age=motion_diagnostic.exporter_age.distribution(),width=motion_diagnostic.sampling_width.distribution();
            event(d::Code::metrics,d::Severity::info,d::ErrorDomain::none,0,"exporter QPC ticks: sample-to-apply count/p50/p99; sampling-width count/p50/p99",
                {sample_age.count,sample_age.p50,sample_age.p99,width.count,width.p50,width.p99});
            complete_metrics();
            last_metrics=now;
        }
    }
    bridge::PluginStatus wire_status(std::int64_t now) noexcept {
        auto status=diagnostic.status;
        status.receive_to_callback=diagnostic.age.distribution();
        status.callback_duration=diagnostic.duration.distribution();
        status.frame_interval=diagnostic.interval.distribution();
        // Keep enable-lifetime identity/counters even before this attachment has a callback.
        if(status.connection_id!=producer.latest.session().connection_id) {
            status.flags=bridge::enabled|bridge::bridge_connected;
            status.connection_id=producer.latest.session().connection_id;
            status.session_revision=status.session_epoch=status.configuration_epoch=status.boost_generation=0;
            status.consumed_sequence.fill(0); status.received_qpc.fill(0); status.receive_to_callback={};
            status.diagnostic_validity&=~bridge::callback_snapshot_valid;
        } else if(status.session_revision!=producer.latest.session().revision) status.flags&=~bridge::observation_ready;
        status.rejected=producer.rejected+view_replies_dropped.load(std::memory_order_relaxed); status.coalesced=producer.coalesced;
        status.generated_qpc=now;
        if(journal) { journal_status=journal->status(); status.events_lost=journal_status.lost; status.persistence_lost=journal_status.persistence_lost; }
        return status;
    }
    void publish() noexcept {
        incoming.writing() = producer;
        if (incoming.publish()) ++producer.coalesced;
    }
    static bool frame(void* context, const bridge::Header& header, const std::uint8_t* p, std::size_t n) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (!self.handshake) {
            bridge::Hello hello;
            if (header.kind != bridge::Kind::hello || !bridge::decode(p,n,hello) ||
                hello.role != bridge::Role::coordinator || hello.qpc_frequency != self.frequency) {
                self.event(d::Code::handshake_rejected,d::Severity::warning,d::ErrorDomain::protocol); return false;
            }
            self.producer.latest.reset(hello.instance_id);
            self.window_gate.reset();self.windows.writing()={};self.windows.publish();
            self.selection_gate.reset();self.complete_gate.reset();self.complete_windows.writing()={};self.complete_windows.publish();
            self.complete_receipt={};
            self.audio_gate.reset();self.audio_states.writing()={};self.audio_states.publish();self.audio_blocked_epoch=0;self.audio_blocked=false;self.audio_transition_sequence=0;
            self.current_connection.store(hello.instance_id,std::memory_order_release);
            self.producer.connected = self.handshake = true;
            self.event(d::Code::connected);
        } else if (header.kind == bridge::Kind::session) {
            bridge::SessionStatus session;
            const auto previous=self.producer.latest.session();
            if (!bridge::decode(p,n,session) || !self.producer.latest.apply(session)) return false;
            if(!motion::same_motion_domain(bridge::history_domain(session),bridge::history_domain(previous))) {
                MotionControl control;control.boundary=true;control.session=session;
                self.publish();
                if(!self.motion_controls.push(control)){self.fail_motion_transport();return false;}
            }
        } else if(header.kind==bridge::Kind::source_selection) {
            MotionControl control;control.source_selection=true;
            const auto previous=self.selection_gate.value();
            if(!bridge::decode(p,n,control.selection)||!self.selection_gate.accept(control.selection,self.producer.latest.session().connection_id))return false;
            if(control.selection.revision!=previous.revision) {
                self.publish();
                if(!self.motion_controls.push(control)){self.fail_motion_transport();return false;}
                if(control.selection.selection_epoch!=previous.selection_epoch) {
                    self.complete_receipt={};
                    self.window_gate.reset();self.complete_gate.reset();
                    self.windows.writing()={};self.windows.publish();self.complete_windows.writing()={};self.complete_windows.publish();
                }
            }
        } else if(header.kind==bridge::Kind::coverage_request) {
            bridge::CoverageRequest request;
            if(!bridge::decode(p,n,request)||request.connection_id!=self.producer.latest.session().connection_id)return false;
            if(self.coverage)self.coverage_requests.push(request);
        } else if(header.kind==bridge::Kind::view_request) {
            bridge::ViewRequest request;
            if(!bridge::decode(p,n,request)||request.connection_id!=self.producer.latest.session().connection_id)return false;
            // Optional view overload cannot retire motion/audio observation. No
            // acknowledgement means unknown delivery; the caller may Query later.
            if(!self.view_requests.push(request)){++self.producer.rejected;self.publish();}
        } else if(header.kind==bridge::Kind::motion_request) {
            MotionControl control;
            if(!bridge::decode(p,n,control.request) || control.request.connection_id!=self.producer.latest.session().connection_id)return false;
            self.publish();
            if(!self.motion_controls.push(control)){self.fail_motion_transport();return false;}
        } else if(header.kind==bridge::Kind::motion_window) {
            bridge::MotionWindow window;
            if(!bridge::decode(p,n,window)||!bridge::matches(window,self.producer.latest.session(),self.selection_gate.value())||
                !self.window_gate.accept(window,self.producer.latest.session()))return false;
            self.windows.writing()=window;self.windows.publish();
        } else if(header.kind==bridge::Kind::complete_pose_window) {
            const auto received=self.receipt_clock(); // Full framed payload is available; before decode/admission.
            const auto lifecycle=self.timing_lifecycle.load(std::memory_order_acquire);
            bridge::CompletePoseWindow window;
            if(!bridge::decode(p,n,window)||!self.complete_gate.accept(window,self.selection_gate.value()))return false;
            self.complete_receipt.scope(window,lifecycle);
            if(window.published_qpc>0&&received>=window.published_qpc)
                self.complete_receipt.samples.add(static_cast<std::uint64_t>(received-window.published_qpc));
            self.complete_windows.writing()={window,received,lifecycle};self.complete_windows.publish();
        } else if(header.kind==bridge::Kind::audio_state) {
            audio::State state;const auto old_epoch=self.audio_gate.value().audio_epoch;
            if(!bridge::decode(p,n,state)||!self.audio_gate.accept(state,self.producer.latest.session().connection_id))self.fail_audio();
            else if(!self.audio_blocked||state.audio_epoch>self.audio_blocked_epoch){
                if(old_epoch!=state.audio_epoch)self.audio_transition_sequence=0;
                self.audio_blocked_epoch=0;self.audio_blocked=false;self.audio_states.writing()=state;self.audio_states.publish();
            }
        } else if(header.kind==bridge::Kind::audio_control) {
            audio::Control control;
            if(!bridge::decode(p,n,control)||control.connection_id!=self.producer.latest.session().connection_id)self.fail_audio();
            else if(!self.audio_blocked&&!self.audio_controls.push(control))self.fail_audio();
        } else if(header.kind==bridge::Kind::audio_transition) {
            audio::Transition transition;
            if(!bridge::decode(p,n,transition)||transition.connection_id!=self.producer.latest.session().connection_id||
                transition.audio_epoch!=self.audio_gate.value().audio_epoch||transition.sequence<=self.audio_transition_sequence)self.fail_audio();
            else if(!self.audio_blocked){self.audio_transition_sequence=transition.sequence;if(!self.audio_transitions.push(transition))self.fail_audio();}
        } else if (header.kind == bridge::Kind::observation) {
            bridge::ObservationMessage observation;
            if (!bridge::decode(p,n,observation)) return false;
            if (!self.producer.latest.apply(observation)) {
                ++self.producer.rejected; self.event(d::Code::observation_rejected,d::Severity::warning,d::ErrorDomain::protocol,
                    0,{}, {static_cast<std::uint64_t>(observation.observation.source),observation.observation.stamp.sequence,0,0,0,0}); return true;
            }
        } else return false;
        self.last_peer = observation_qpc();
        return true;
    }
    void fail_audio() noexcept {
        audio_blocked_epoch=audio_gate.value().audio_epoch;audio_blocked=true;audio_fault_generation.fetch_add(1,std::memory_order_release);++producer.rejected;
    }
    bool stopped() const noexcept { return WaitForSingleObject(stop_event.value,0) == WAIT_OBJECT_0; }
    void connection(HANDLE pipe) noexcept {
        producer.peer={};producer.authenticated=false;
        if(pipe)producer.authenticated=non_normal::authenticate(pipe,false,{},producer.peer);
        else {const auto peer=psx::sync::runtime()->status().peer;producer.peer={peer.pid,peer.process_birth};}
        Handle read_event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!read_event.value) { event(d::Code::io_failure,d::Severity::error,d::ErrorDomain::win32,GetLastError(),"read event"); return; }
        Handle write_event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!write_event.value) { event(d::Code::io_failure,d::Severity::error,d::ErrorDomain::win32,GetLastError(),"write event"); return; }
        OVERLAPPED read{}, write{}; read.hEvent=read_event.value; write.hEvent=write_event.value;
        bool reading=false, writing=false, hello_sent=false;
        DWORD write_size{};
        std::array<std::uint8_t,bridge::max_frame> input{};
        bridge::Frame output;
        std::uint32_t sequence{};
        const auto attached = observation_qpc();
        auto last_status = attached - static_cast<std::int64_t>(frequency);
        last_peer = attached; handshake=false; framer.reset();
        while (!stopped()) {
            if(!psx::sync::worker_poll(pipe))break;
            const auto now=observation_qpc();
            service_diagnostics(now);
            if(audio_outgoing.consume())audio_diagnostic=audio_outgoing.reading();
            if(motion_fault.load(std::memory_order_acquire))break;
            if (now-last_peer >= static_cast<std::int64_t>(frequency*5)) {
                event(d::Code::disconnected,d::Severity::warning,d::ErrorDomain::win32,WAIT_TIMEOUT,"peer timeout"); break;
            }
            if (!writing && !hello_sent) {
                bridge::Hello hello; hello.role=bridge::Role::plugin; hello.instance_id=identity();
                if (!hello.instance_id) hello.instance_id=1;
                hello.qpc_frequency=frequency;
                const char* build=coverage?"psx-xplane-" PSX_PLUGIN_VERSION ";cov1;view1":"psx-xplane-" PSX_PLUGIN_VERSION ";view1";
                std::copy_n(build,std::strlen(build)+1,hello.build.begin());
                if (!bridge::encode(hello,++sequence,output)) break;
                hello_sent=true;
                write_size=static_cast<DWORD>(output.size);
            } else if(!writing && handshake && prepare_motion_ack(sequence,output)) {
                write_size=static_cast<DWORD>(output.size);
            } else if(!writing && handshake && prepare_coverage_reply(sequence,output)) {
                write_size=static_cast<DWORD>(output.size);
            } else if(!writing && handshake && prepare_view_reply(sequence,output)) {
                write_size=static_cast<DWORD>(output.size);
            } else if(!writing&&handshake&&elapsed(now,last_audio,config.status_ms)&&audio_diagnostic.connection_id==producer.latest.session().connection_id&&audio::valid(audio_diagnostic)){
                if(!bridge::encode(audio_diagnostic,++sequence,output))break;
                write_size=static_cast<DWORD>(output.size);last_audio=now;
            } else if (!writing && handshake && elapsed(now,last_status,config.status_ms)) {
                const auto status=wire_status(now);
                if (!bridge::encode(status,++sequence,output)) break;
                write_size=static_cast<DWORD>(output.size); last_status=now;
            } else if(!writing && handshake && elapsed(now,last_motion,config.status_ms) && motion_diagnostic.status.plugin_load_id &&
                      motion_diagnostic.status.connection_id==producer.latest.session().connection_id) {
                auto status=motion_diagnostic.status;status.generated_qpc=now;
                status.pre_callback_duration=motion_diagnostic.pre.distribution();status.post_callback_duration=motion_diagnostic.post.distribution();
                status.boost_receive_to_apply=motion_diagnostic.age.distribution();status.pitch_age=motion_diagnostic.pitch.distribution();status.frame_interval=motion_diagnostic.interval.distribution();
                status.output_age=motion_diagnostic.output_age.distribution();status.hold_duration=motion_diagnostic.hold_duration.distribution();
                status.exporter_sample_to_apply=motion_diagnostic.exporter_age.distribution();status.sampling_width=motion_diagnostic.sampling_width.distribution();
                if(!bridge::encode(status,++sequence,output))break;
                write_size=static_cast<DWORD>(output.size);last_motion=now;
            } else if (!writing && handshake && journal && elapsed(now,last_events,config.status_ms)) {
                const auto recent=journal->recent_since(recent_cursor); recent_cursor=recent.cursor; last_events=now;
                if(recent.overwritten) event(d::Code::events_lost,d::Severity::warning,d::ErrorDomain::none,0,"bridge recent history overwritten");
                bridge::DiagnosticBatch batch; batch.connection_id=producer.latest.session().connection_id;
                batch.count=recent.count; batch.events=recent.events;
                // Event loss/encoding cannot invalidate the transport or its source state.
                if(batch.count && bridge::encode(batch,++sequence,output)) write_size=static_cast<DWORD>(output.size);
            }
            if (!writing && write_size) {
                ResetEvent(write.hEvent); DWORD sent{};
                if (psx::sync::io_write(pipe,output.bytes.data(),write_size,&sent,&write)) {
                    if (sent!=write_size) { event(d::Code::io_failure,d::Severity::error,d::ErrorDomain::win32,ERROR_WRITE_FAULT,"short pipe write"); break; }
                    write_size=0;
                } else { const auto error=GetLastError(); if(error==ERROR_IO_PENDING) writing=true;
                    else { event(d::Code::io_failure,d::Severity::warning,d::ErrorDomain::win32,error,"pipe write"); break; } }
            }
            if (!reading) {
                ResetEvent(read.hEvent); DWORD got{};
                if (psx::sync::io_read(pipe,input.data(),static_cast<DWORD>(input.size()),&got,&read)) {
                    if(!got) { event(d::Code::disconnected); break; }
                    if (!framer.feed(input.data(),got,frame,this)) { ++producer.rejected; event(d::Code::frame_rejected,d::Severity::warning,d::ErrorDomain::protocol); break; }
                    publish();
                    // Fall through to service output/deadlines; never drain reads unbounded.
                } else { const auto error=GetLastError(); if(error==ERROR_IO_PENDING) reading=true;
                    else { event(d::Code::io_failure,d::Severity::warning,d::ErrorDomain::win32,error,"pipe read"); break; } }
            }
            if (writing && WaitForSingleObject(write.hEvent,0)==WAIT_OBJECT_0) {
                DWORD sent{};
                if (!psx::sync::io_result(pipe,&write,&sent,FALSE)) { const auto error=GetLastError(); event(d::Code::io_failure,d::Severity::warning,d::ErrorDomain::win32,error,"write completion"); break; }
                if(sent!=write_size) { event(d::Code::io_failure,d::Severity::error,d::ErrorDomain::win32,ERROR_WRITE_FAULT,"short pipe completion"); break; }
                writing=false; write_size=0;
            }
            if (reading && WaitForSingleObject(read.hEvent,0)==WAIT_OBJECT_0) {
                DWORD got{};
                if (!psx::sync::io_result(pipe,&read,&got,FALSE)) { const auto error=GetLastError(); event(d::Code::io_failure,d::Severity::warning,d::ErrorDomain::win32,error,"read completion"); break; }
                reading=false;
                if(!got) { event(d::Code::disconnected); break; }
                if (!framer.feed(input.data(),got,frame,this)) { ++producer.rejected; event(d::Code::frame_rejected,d::Severity::warning,d::ErrorDomain::protocol); break; }
                publish();
            }
            // Immediate reads loop without a sleep. Pending events sleep at most
            // 10ms for diagnostics/deadlines, and stop always wakes immediately.
            if (reading) {
                HANDLE waits[]{stop_event.value,read.hEvent,write.hEvent};
                WaitForMultipleObjects(writing ? 3 : 2,waits,FALSE,10);
            }
        }
        psx::sync::io_cancel(pipe,nullptr);
        DWORD ignored{};
        if (reading) psx::sync::io_result(pipe,&read,&ignored,TRUE);
        if (writing) psx::sync::io_result(pipe,&write,&ignored,TRUE);
        event(d::Code::disconnected,d::Severity::info,d::ErrorDomain::none,0,stopped() ? "stop requested" : "attachment ended");
        current_connection.store(0,std::memory_order_release);
        producer.connected=false;producer.authenticated=false;producer.peer={}; producer.latest.reset(); publish();
        window_gate.reset();windows.writing()={};windows.publish();
        selection_gate.reset();complete_gate.reset();complete_windows.writing()={};complete_windows.publish();
        if(!stopped())complete_receipt={}; // Orderly stop retains the last scope for the final worker summary.
        audio_gate.reset();audio_states.writing()={};audio_states.publish();audio_blocked_epoch=0;audio_blocked=false;audio_transition_sequence=0;
        motion_fault.store(false,std::memory_order_release); // Next attachment resynchronizes; SDK notice has a separate durable generation.
    }
    bool prepare_view_reply(std::uint32_t& sequence,bridge::Frame& output) noexcept {
        bridge::ViewReply reply;
        for(std::size_t i=0;i<64&&view_replies.pop(reply);++i)
            if(reply.connection_id==producer.latest.session().connection_id)return bridge::encode(reply,++sequence,output);
        return false;
    }
    bool prepare_motion_ack(std::uint32_t& sequence,bridge::Frame& output) noexcept {
        bridge::MotionAck ack;
        // Disconnected operations are ambiguous to the peer. Never replay them on a new connection.
        for(std::size_t i=0;i<64 && motion_acks.pop(ack);++i)
            if(ack.connection_id==producer.latest.session().connection_id)return bridge::encode(ack,++sequence,output);
        return false;
    }
    void run() noexcept {while(!stopped()){if(psx::sync::enabled()){psx::sync::WorkerIo io(psx::sync::Family::observation);connection(nullptr);}else run_local();WaitForSingleObject(stop_event.value,20);}service_diagnostics(observation_qpc());complete_metrics(true);}
    void run_local() noexcept {
        if(endpoint.empty())return;
        DWORD retry=1000;
        while (psx::sync::local_io_selected()&&!stopped()) {
            {
                Handle pipe(CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
                if (pipe.value!=INVALID_HANDLE_VALUE) { connection(pipe.value); if (handshake) retry=1000; }
                else { const auto error=GetLastError(); event(d::Code::retry,
                    error==ERROR_FILE_NOT_FOUND || error==ERROR_PIPE_BUSY ? d::Severity::info : d::Severity::warning,
                    d::ErrorDomain::win32,error,"pipe attachment",{retry,0,0,0,0,0}); }
            } // Close failed attachment before waiting to retry.
            const auto retry_started=observation_qpc();
            do { service_diagnostics(observation_qpc());
                if(WaitForSingleObject(stop_event.value,10)==WAIT_OBJECT_0) break;
            } while(psx::sync::local_io_selected()&&!elapsed(observation_qpc(),retry_started,retry));
            retry=(std::min)(retry*2,DWORD{8000});
        }
        service_diagnostics(observation_qpc());
        complete_metrics(true); // stop() published the final owner samples before signaling/joining.
    }
    void invalidate(const ObservationLifecycle& life) noexcept {
        consumer.lifecycle_generation=timing_lifecycle.fetch_add(1,std::memory_order_acq_rel)+1;
        consumer.complete_consumption={};
        auto& status=consumer.status;
        status.flags&=~bridge::observation_ready;
        status.diagnostic_validity&=~bridge::callback_snapshot_valid;
        status.consumed_sequence.fill(0); status.received_qpc.fill(0);
        status.aircraft_generation=life.aircraft_generation; status.scenery_generation=life.scenery_generation;
        status.aircraft=life.aircraft; status.aircraft.back()=0;
        consumer.age={}; lifecycle_invalidated=true;
        outgoing.writing()=consumer;
        if(outgoing.publish()) ++status.diagnostics_dropped;
    }
    const bridge::PluginStatus& callback(const ObservationLifecycle& life) noexcept {
        const auto begin=callback_clock();
        if(consumer.status.aircraft_generation!=life.aircraft_generation ||
           consumer.status.scenery_generation!=life.scenery_generation || consumer.status.aircraft!=life.aircraft) invalidate(life);
        const bool lifecycle_ready=life.aircraft[0]!=0 && !lifecycle_invalidated;
        lifecycle_invalidated=false;
        incoming.consume();windows.consume();consume_complete();audio_states.consume();
        const auto& snapshot=incoming.reading();
        const auto& session=snapshot.latest.session();
        auto& status=consumer.status;
        if (status.callback_qpc && begin>=status.callback_qpc) {
            const auto interval=static_cast<std::uint64_t>(begin-status.callback_qpc);
            consumer.interval.add(interval);
            status.frame_interval_max.add(interval,status.callback_count+1,begin,status.callback_qpc);
            if(interval>=frequency*config.gap_ms/1000) {
                d::Event gap; gap.code=d::Code::callback_gap; gap.component=d::Component::plugin;
                gap.context=context(status); gap.qpc=begin;
                gap.values={interval,status.callback_count+1,static_cast<std::uint64_t>(status.callback_qpc),static_cast<std::uint64_t>(begin),0,0};
                sdk_events.emit(gap);
            }
        }
        const bool reset=status.connection_id!=session.connection_id || status.session_epoch!=session.session_epoch;
        if (reset) { status.consumed_sequence.fill(0); status.received_qpc.fill(0); consumer.age={}; }
        status.connection_id=session.connection_id; status.session_revision=session.revision;
        status.session_epoch=session.session_epoch; status.configuration_epoch=session.configuration_epoch;
        status.boost_generation=session.boost_generation;
        status.aircraft_generation=life.aircraft_generation; status.scenery_generation=life.scenery_generation;
        status.aircraft=life.aircraft; status.aircraft.back()=0;
        ++status.callback_count; status.callback_qpc=begin;
        status.diagnostic_validity=bridge::counter_baseline_valid|bridge::callback_snapshot_valid;
        status.flags=bridge::enabled;
        status.rejected=snapshot.rejected; status.coalesced=snapshot.coalesced;
        if (snapshot.connected) status.flags|=bridge::bridge_connected;
        // PSX version evidence is informational; semantic/bootstrap validity
        // and the actual available observations determine readiness.
        constexpr auto ready=bridge::main_connected|bridge::synchronized|bridge::boost_connected|bridge::boost_available;
        if (lifecycle_ready && snapshot.connected && (session.flags&ready)==ready && session.family!=aircraft::EngineFamily::unknown &&
            snapshot.latest.available()[static_cast<std::size_t>(aircraft::SourceRecord::boost)]) status.flags|=bridge::observation_ready;
        for (std::size_t i=0;i<bridge::source_slots;++i) {
            if (!lifecycle_ready || !snapshot.connected || !snapshot.latest.available()[i]) {
                status.consumed_sequence[i]=0; status.received_qpc[i]=0; continue;
            }
            const auto& stamp=snapshot.latest.records()[i].observation.stamp;
            if ((status.consumed_sequence[i]!=stamp.sequence || status.received_qpc[i]!=stamp.received_qpc) && begin>=stamp.received_qpc) {
                const auto age=static_cast<std::uint64_t>(begin-stamp.received_qpc);
                consumer.age.add(age); status.receive_to_callback_max.add(age,status.callback_count,begin,stamp.received_qpc);
            }
            status.consumed_sequence[i]=stamp.sequence; status.received_qpc[i]=stamp.received_qpc;
        }
        outgoing.writing()=consumer;
        if (outgoing.publish()) ++status.diagnostics_dropped;
        // A completed duration is included in the NEXT diagnostic publication,
        // so the measured span includes the handoff itself.
        const auto end=callback_clock();
        if(end>=begin) {
            const auto duration=static_cast<std::uint64_t>(end-begin);
            consumer.duration.add(duration); status.callback_duration_max.add(duration,status.callback_count,begin);
            status.duration_completed_callback=status.callback_count;
        }
        return status;
    }
};
ObservationClient::ObservationClient() noexcept = default;
ObservationClient::~ObservationClient() { stop(); }
bool ObservationClient::start(const ObservationOptions& options) noexcept {
    if (impl_) return true;
    try {
        auto endpoint=options.endpoint.empty() ? observation_endpoint() : options.endpoint;
        if(endpoint.empty()){if(!psx::sync::runtime())return false;}
        else if (!endpoint.starts_with(L"\\\\.\\pipe\\") || endpoint.size()<=9 || endpoint.size()>240 ||
                 endpoint.find(L'\0')!=std::wstring::npos) return false;
        auto implementation=std::make_unique<Impl>(std::move(endpoint),options);
        if (!implementation->stop_event.value || !implementation->frequency) return false;
        try{if(implementation->coverage)implementation->coverage_thread=std::thread([p=implementation.get()]{p->coverage_run();});}catch(...){implementation->coverage.reset();}
        implementation->worker=std::thread([p=implementation.get()] { p->run(); });
        impl_=std::move(implementation);
        return true;
    } catch (...) { return false; }
}
void ObservationClient::stop() noexcept {
    if (!impl_) return;
    // Owner is quiescent. Include the last completed duration before worker retirement.
    impl_->outgoing.writing()=impl_->consumer;
    if(impl_->outgoing.publish()) ++impl_->consumer.status.diagnostics_dropped;
    SetEvent(impl_->stop_event.value);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->coverage_thread.joinable()) impl_->coverage_thread.join();
    const auto& status=impl_->consumer.status;
    impl_->sdk_events.emit(d::Code::metrics,d::Severity::info,impl_->context(status),d::ErrorDomain::none,0,"enable final",
        {status.callback_count,status.callback_duration_max.ticks,status.frame_interval_max.ticks,status.receive_to_callback_max.ticks,
         status.duration_completed_callback,status.diagnostics_dropped});
    impl_->sdk_events.emit(d::Code::metrics,d::Severity::info,impl_->context(status),d::ErrorDomain::none,0,"final receive maximum",maximum_values(status.receive_to_callback_max,0));
    impl_->sdk_events.emit(d::Code::metrics,d::Severity::info,impl_->context(status),d::ErrorDomain::none,0,"final duration maximum",maximum_values(status.callback_duration_max,1));
    impl_->sdk_events.emit(d::Code::metrics,d::Severity::info,impl_->context(status),d::ErrorDomain::none,0,"final interval maximum",maximum_values(status.frame_interval_max,2));
    impl_.reset(); stopped_={};
}
void ObservationClient::invalidate_lifecycle(const ObservationLifecycle& life) noexcept { if(impl_) impl_->invalidate(life); }
const bridge::PluginStatus& ObservationClient::callback(const ObservationLifecycle& life) noexcept {
    return impl_ ? impl_->callback(life) : stopped_;
}
const bridge::PluginStatus& ObservationClient::status() const noexcept { return impl_ ? impl_->consumer.status : stopped_; }
const bridge::LatestState* ObservationClient::latest() const noexcept { return impl_ ? &impl_->incoming.reading().latest : nullptr; }
const bridge::MotionWindow* ObservationClient::motion_window() const noexcept {return impl_&&connected()?&impl_->windows.reading():nullptr;}
const bridge::CompletePoseWindow* ObservationClient::complete_window() const noexcept {return impl_&&connected()?&impl_->complete_windows.reading().window:nullptr;}
void ObservationClient::refresh() noexcept {if(impl_){impl_->incoming.consume();impl_->windows.consume();impl_->consume_complete();impl_->audio_states.consume();}}
bool ObservationClient::connected() const noexcept {return impl_ && impl_->incoming.reading().connected && impl_->current_connection.load(std::memory_order_acquire)==impl_->incoming.reading().latest.session().connection_id;}
non_normal::ProcessIdentity ObservationClient::selected_peer() const noexcept {return connected()?impl_->incoming.reading().peer:non_normal::ProcessIdentity{};}
non_normal::ProcessIdentity ObservationClient::authenticated_peer() const noexcept {return impl_&&connected()&&impl_->incoming.reading().authenticated?impl_->incoming.reading().peer:non_normal::ProcessIdentity{};}
bool ObservationClient::pop_view_request(bridge::ViewRequest& request) noexcept {return impl_&&impl_->view_requests.pop(request);}
bool ObservationClient::submit_view_reply(const bridge::ViewReply& reply) noexcept {
    if(!impl_)return false;if(impl_->view_replies.push(reply))return true;
    impl_->view_replies_dropped.fetch_add(1,std::memory_order_relaxed);return false;
}
bool ObservationClient::pop_motion_control(MotionControl& control) noexcept {return impl_&&impl_->motion_controls.pop(control);}
bool ObservationClient::submit_motion_ack(const bridge::MotionAck& ack) noexcept {
    if(!impl_)return false;if(impl_->motion_acks.push(ack))return true;impl_->fail_motion_transport();return false;
}
void ObservationClient::publish_motion(const MotionReport& report) noexcept {
    if(!impl_)return;impl_->motion_outgoing.writing()=report;impl_->motion_outgoing.writing().status.trace_dropped+=impl_->motion_trace_dropped;
    if(impl_->motion_outgoing.publish())++impl_->motion_trace_dropped;
}
void ObservationClient::publish_environment_timing(const EnvironmentTiming& timing) noexcept {
    if(!impl_||!timing.generation)return;
    // The existing SDK producer is wait-free and participates in the journal's
    // loss/cutoff accounting. Workers still format, persist and transport it.
    for(unsigned stage=0;stage<timing.stages.size();++stage){const auto& slot=timing.stages[stage];if(!slot.count)continue;
        d::Event event;event.context.plugin_load_id=impl_->load_id;event.context.connection_id=timing.connection;
        event.code=d::Code::message;event.component=d::Component::sdk;event.error=stage|(slot.groups<<8);
        event.values={static_cast<std::uint64_t>(slot.begin),static_cast<std::uint64_t>(slot.end),slot.callback,slot.source,slot.target,timing.generation};
        d::text(event.text,"environment_timing_v1");impl_->sdk_events.emit(event);
    }
    d::Context context;context.plugin_load_id=impl_->load_id;context.connection_id=timing.connection;
    const auto count=[&](EnvironmentStage stage){return timing.stages[static_cast<unsigned>(stage)].count;};
    impl_->sdk_events.emit(d::Code::message,d::Severity::info,context,d::ErrorDomain::none,0,"environment_timing_counts_v1",
        {count(EnvironmentStage::regional),count(EnvironmentStage::settings),count(EnvironmentStage::clock),count(EnvironmentStage::readback),count(EnvironmentStage::restoration),timing.generation});
}
bool ObservationClient::motion_transport_fault() const noexcept {
    if(!impl_)return false;const auto generation=impl_->motion_fault_generation.load(std::memory_order_acquire);
    if(generation==impl_->motion_fault_seen)return false;impl_->motion_fault_seen=generation;return true;
}
void ObservationClient::request_profile(const ProfileRequest& request) noexcept {if(impl_){impl_->profile_requests.writing()=request;impl_->profile_requests.publish();}}
bool ObservationClient::poll_profile(MotionProfile& profile) noexcept {if(!impl_||!impl_->profiles.consume())return false;profile=impl_->profiles.reading();return true;}
const audio::State* ObservationClient::audio_state() const noexcept {return impl_&&connected()?&impl_->audio_states.reading():nullptr;}
bool ObservationClient::pop_audio_control(audio::Control& c) noexcept {return impl_&&impl_->audio_controls.pop(c);}
bool ObservationClient::pop_audio_transition(audio::Transition& t) noexcept {return impl_&&impl_->audio_transitions.pop(t);}
void ObservationClient::publish_audio(const audio::Status& s) noexcept {if(impl_){impl_->audio_outgoing.writing()=s;impl_->audio_outgoing.publish();}}
bool ObservationClient::audio_transport_fault() noexcept {if(!impl_)return false;const auto g=impl_->audio_fault_generation.load(std::memory_order_acquire);if(g==impl_->audio_fault_seen)return false;impl_->audio_fault_seen=g;return true;}
} // namespace psx::xp
