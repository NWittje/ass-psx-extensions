#include <du_client/diagnostics.hpp>
#include <psx/display/tcp.hpp>
#include <fstream>
namespace du_client
{
static_assert(sizeof(psx::norm::DiagnosticQueue)*3+sizeof(Diagnostics)+sizeof(FailureEvidence)+2*sizeof(FailureSnapshot)+8*sizeof(FailureRecord)+8192<1024*1024,
              "Client diagnostic queues exceed the independent 1 MiB bound");
Diagnostics::Diagnostics(std::filesystem::path output) : output_(std::move(output))
{
    if (std::filesystem::exists(output_))
        throw std::runtime_error("Qualification output must be a fresh directory");
    std::filesystem::create_directories(output_);
    trace_=std::make_shared<psx::du::transport::Trace>(output_/L"transport",3);
    worker_ = std::thread(&Diagnostics::worker, this);
}
Diagnostics::~Diagnostics()
{
    {
        std::lock_guard l(mutex_);
        quit_ = true;
    }
    ready_.notify_one();
    worker_.join();
}
void Diagnostics::sample(const View &v, uint64_t present, double render_ms, uint64_t render_frame_no)
{
    // Submission return is recorded at its actual UI call site. This sample
    // records its own post-presentation observation, not another render return.
    std::unique_lock l(mutex_, std::try_to_lock);
    if (!l || head_ - tail_ == queue_.size())
    {
        ++lost_;
        return;
    }
    unsigned metadata=4;
    if(v.identity){
        if(v.identity->size()>4096){++lost_;return;}
        for(unsigned n=0;n<4;++n)if(metadata_[n].bytes==v.identity->size()&&std::equal(v.identity->begin(),v.identity->end(),metadata_[n].json.begin())){metadata=n;break;}
        if(metadata==4)for(unsigned n=0;n<4;++n)if(!metadata_[n].references){metadata=n;metadata_[n].bytes=v.identity->size();std::copy(v.identity->begin(),v.identity->end(),metadata_[n].json.begin());break;}
        if(metadata==4){++lost_;return;}
        ++metadata_[metadata].references;
    }
    auto &s = queue_[head_++ % queue_.size()];
    s = {v.connection_epoch,  v.frame->header.sequence,
         v.frame->header.end, v.frame_received,
         v.last_decoded,      present,
         v.raw_replaced,      v.decoded_replaced,
         v.source_lost,       v.invalid,
         render_ms,           v.source_pid,
         v.source_creation,render_frame_no,metadata,v.frame->header.frequency,v.same_clock};
    s.cpu_budget_rejected=v.cpu_budget_rejected;
    s.cpu_payload_bytes=psx::du::transport::tcp::model_bytes();
    s.norm_payload_bytes=0;
    s.norm_allocation_denied=0;
    l.unlock();
    ready_.notify_one();
}
void Diagnostics::worker()
{
    try
    {
        std::ofstream frames(output_ / L"timing.jsonl");
        if (!frames)
            throw std::runtime_error("Qualification output unavailable");
        uint64_t count = 0, last_epoch = 0;
        for (;;)
        {
            Sample s;
            std::array<char,4096> identity{};size_t identity_bytes{};
            {
                std::unique_lock l(mutex_);
                ready_.wait(l, [&] { return quit_ || tail_ != head_; });
                if (tail_ == head_ && quit_)
                    break;
                s = std::move(queue_[tail_++ % queue_.size()]);
                if(s.metadata_slot<4){auto&m=metadata_[s.metadata_slot];if(s.epoch!=last_epoch){identity=m.json;identity_bytes=m.bytes;}--m.references;}
            }
            if (s.epoch != last_epoch)
            {
                std::ofstream meta(output_ / (L"source-" + std::to_wstring(s.epoch) + L".json"));
                meta << "{\"pid\":" << s.pid << ",\"creation_filetime\":" << s.creation
                     << ",\"identity\":";
                if(identity_bytes)meta.write(identity.data(),identity_bytes);else meta<<"null";
                meta<<"}\n";
                if (!meta)
                    throw std::runtime_error("Qualification metadata write failed");
                last_epoch = s.epoch;
            }
            auto ms = [](uint64_t a, uint64_t b)
            { return b >= a ? double(b - a) * 1000 / frequency() : -1.; };
            frames << "{\"epoch\":" << s.epoch << ",\"sequence\":" << s.sequence
                   << ",\"qpc_frequency\":" << frequency() << ",\"source_end\":" << s.source_end
                   << ",\"received\":" << s.receive << ",\"decoded\":" << s.decode
                   << ",\"post_present_return\":" << s.present
                   << ",\"source_frequency\":" << s.source_frequency
                   << ",\"same_pc_clock\":" << (s.same_clock?"true":"false")
                   << ",\"source_to_validated_delivery_ms\":" << (s.same_clock?std::to_string(ms(s.source_end,s.receive)):"null")
                   << ",\"source_to_post_present_return_ms\":" << (s.same_clock?std::to_string(ms(s.source_end,s.present)):"null")
                   << ",\"receive_to_decode_ms\":" << ms(s.receive, s.decode)
                   << ",\"decode_to_post_present_return_ms\":" << ms(s.decode, s.present)
                   << ",\"ass_render_frame_number\":" << s.render_frame_no
                   << ",\"ass_render_wall_ms\":" << s.render_ms << ",\"raw_replaced\":" << s.raw_replaced
                   << ",\"decoded_replaced\":" << s.decoded_replaced << ",\"source_lost\":" << s.source_lost
                   << ",\"invalid\":" << s.invalid
                   << ",\"cpu_budget_rejected\":" << s.cpu_budget_rejected
                   << ",\"joint_rx_model_payload_bytes\":" << s.cpu_payload_bytes
                   << ",\"norm_payload_bytes\":" << s.norm_payload_bytes
                   << ",\"norm_allocation_denied\":" << s.norm_allocation_denied << "}\n";
            if (!frames)
                throw std::runtime_error("Qualification timing write failed");
            ++count;
        }
        frames.close();
        if (!frames)
            throw std::runtime_error("Qualification flush failed");
        std::ofstream summary(output_ / L"summary.json");
        summary << "{\"schema\":2,\"frames\":" << count << ",\"diagnostic_records_lost\":" << lost_
                << ",\"presentation_boundary\":\"Application post-present return; ASS submission measured separately in stage records; "
                   "physical "
                   "scanout unmeasured\",\"ordinary_automatic_capture\":false}\n";
        if (!summary)
            throw std::runtime_error("Qualification summary write failed");
    }
    catch (...)
    {
        failed_ = true;
    }
}
} // namespace du_client
