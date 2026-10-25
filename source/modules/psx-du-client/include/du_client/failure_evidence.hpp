#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
namespace du_client {
enum class FailureStage : unsigned { admission,decode,replay,presentation };
struct FailureRecord {
    uint64_t revision{},qpc{},epoch{},sequence{},paint{},generation{},source_created{},count{};
    uint32_t source_pid{},unit{},error{};
    FailureStage stage{};
    bool recovery{},truncated{};
    std::array<wchar_t,256> text{};
};
struct FailureState { uint64_t rejected{},recoveries{};bool active{};FailureRecord last; };
struct FailureSnapshot { std::array<FailureState,4> stages;uint64_t revision{},lost{}; };
// Runtime's existing mutex protects all producers and this bounded transition
// queue. Repeated identical errors only advance counts; the worker owns JSON/I/O.
class FailureEvidence {
    FailureSnapshot state_;
    std::array<FailureRecord,8> queue_{};
    uint64_t head_{},tail_{};
    void push(FailureRecord r) noexcept {
        r.revision=++state_.revision;
        if(head_-tail_==queue_.size()){++state_.lost;return;}
        queue_[head_++%queue_.size()]=r;
    }
public:
    void reject(FailureRecord r,std::wstring_view message,uint64_t count=1) noexcept {
        auto& s=state_.stages[unsigned(r.stage)];s.rejected+=count;r.count=s.rejected;
        auto n=std::min(message.size(),r.text.size()-1);
        std::copy_n(message.data(),n,r.text.data());r.truncated=message.size()>n;
        const bool changed=!s.active||s.last.epoch!=r.epoch||s.last.error!=r.error||
                           s.last.text!=r.text||s.last.truncated!=r.truncated;
        s.active=true;s.last=r;if(changed)push(r);
    }
    void recover(FailureRecord r) noexcept {
        auto& s=state_.stages[unsigned(r.stage)];if(!s.active)return;
        s.active=false;++s.recoveries;r.recovery=true;r.count=s.rejected;push(r);
    }
    FailureSnapshot snapshot()const noexcept{return state_;}
    size_t drain(std::array<FailureRecord,8>& out) noexcept {
        size_t n=0;while(tail_<head_)out[n++]=queue_[tail_++%queue_.size()];return n;
    }
};
std::string failure_json(const FailureRecord&);
}
