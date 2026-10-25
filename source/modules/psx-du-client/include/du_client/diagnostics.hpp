#pragma once
#include "runtime.hpp"
#include <filesystem>
namespace du_client
{
class Diagnostics
{
    struct Sample
    {
        uint64_t epoch{}, sequence{}, source_end{}, receive{}, decode{}, present{}, raw_replaced{},
            decoded_replaced{}, source_lost{}, invalid{};
        double render_ms{};
        uint32_t pid{};
        uint64_t creation{}, render_frame_no{};
        uint32_t metadata_slot{4};
        uint64_t source_frequency{};
        bool same_clock{};
        uint64_t cpu_budget_rejected{}, cpu_payload_bytes{}, norm_payload_bytes{}, norm_allocation_denied{};
    };
    std::array<Sample, 128> queue_;
    struct Metadata {std::array<char,4096> json{};size_t bytes{};unsigned references{};};
    std::array<Metadata,4> metadata_;
    std::mutex mutex_;
    std::condition_variable ready_;
    size_t head_{}, tail_{};
    bool quit_{};
    std::thread worker_;
    std::filesystem::path output_;
    std::atomic<uint64_t> lost_{};
    std::atomic<bool> failed_{};
    std::shared_ptr<psx::du::transport::Trace> trace_;
    void worker();

  public:
    explicit Diagnostics(std::filesystem::path);
    ~Diagnostics();
    void sample(const View &, uint64_t present, double render_ms, uint64_t render_frame_no);
    auto trace()const {return trace_;}
    bool failed() const
    {
        return failed_||trace_->failed();
    }
};
} // namespace du_client
