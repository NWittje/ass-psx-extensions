#include <du_client/trim.hpp>
#include <algorithm>

namespace du_client
{
using namespace psx::du::transport;
TrimReceiver::TrimReceiver(bool force_prepare_failure)
{
    try
    {
        if(force_prepare_failure)throw std::bad_alloc();
        for (auto &s : slots_) s.reserve(psx::instrument::frame_limit);
        discard_.reserve(psx::instrument::frame_limit);
        decoder_ = std::thread(&TrimReceiver::decode, this);
        prepared_=true;
    }
    catch (...)
    {
        for(auto&s:slots_)std::vector<uint8_t>().swap(s);
        std::vector<uint8_t>().swap(discard_);
        try{view_.status=L"STAB TRIM preparation unavailable - main DU continues";}catch(...){}
    }
}
TrimReceiver::~TrimReceiver() { shutdown(); join(); }
bool TrimReceiver::enable(bool enabled)
{
    std::lock_guard lock(mutex_);
    if(!prepared_)return false;
    if (view_.enabled == enabled) return enabled;
    view_.enabled = enabled;active_generation_=0;active_session_={}; ++view_.epoch; view_.frame.reset(); pending_ = -1;
    view_.received = 0; view_.connected = false;view_.observation_current=false; view_.status.clear();
    ready_.notify_all();return enabled;
}
void TrimReceiver::reset()
{
    std::lock_guard lock(mutex_);if(!prepared_)return;active_generation_=0;active_session_={}; ++view_.epoch; view_.frame.reset(); pending_ = -1;
    view_.received = 0; view_.connected = false;view_.observation_current=false; view_.status.clear();
}
void TrimReceiver::shutdown() { std::lock_guard lock(mutex_); quit_ = true; ready_.notify_all(); }
void TrimReceiver::join() { if (decoder_.joinable()) decoder_.join(); }
std::span<uint8_t> TrimReceiver::acquire(size_t size)
{
    if(!prepared_)throw std::runtime_error("Instrument preparation unavailable");
    if (size > psx::instrument::frame_limit) throw std::runtime_error("Instrument raw frame limit");
    std::lock_guard lock(mutex_);
    if (assembling_ >= 0 || discarding_) throw std::runtime_error("Concurrent instrument producer");
    if (!view_.enabled || quit_)
    {
        discard_.resize(size); discarding_ = true; return discard_;
    }
    int slot = working_ == 0 ? 1 : 0;
    if (pending_ == slot) { pending_ = -1; ++view_.replaced; }
    slots_[slot].resize(size); assembling_ = slot; epochs_[slot] = view_.epoch;
    return slots_[slot];
}
void TrimReceiver::abort() { std::lock_guard lock(mutex_); assembling_ = -1; discarding_ = false; }
tcp::Disposition TrimReceiver::publish(const InstrumentEnvelope &e, uint64_t completed,unsigned connection)
{
    auto d = psx::instrument::decode(e.frame);
    std::lock_guard lock(mutex_);
    if (discarding_) { discarding_ = false; ++view_.budget_rejected;view_.observation_current=false; return tcp::Disposition::budget; }
    const int slot = assembling_; assembling_ = -1;
    if (slot < 0 || epochs_[slot] != view_.epoch || !view_.enabled || quit_)
        return tcp::Disposition::budget;
    if (e.frame.data() != slots_[slot].data() || d.instrument_id != e.instrument)
        throw std::runtime_error("Instrument raw ownership/source mismatch");
    if(active_session_!=e.session||active_generation_!=e.generation){
        active_session_=e.session;active_generation_=e.generation;++view_.epoch;
        epochs_[slot]=view_.epoch;pending_=-1;view_.observation_current=false;
    }
    const bool replaced = pending_ >= 0;
    if (replaced) ++view_.replaced;
    origins_[slot] = e.source; received_[slot] = completed; pending_ = slot;
    correlations_[slot]={e.session,e.generation,e.sequence,d.sequence};connections_[slot]=connection;
    view_.available = view_.connected = true; view_.status.clear(); ready_.notify_one();
    return replaced ? tcp::Disposition::replaced : tcp::Disposition::accepted;
}
void TrimReceiver::offer(Owned bytes, const LocalView &v)
{
    try
    {
        auto raw = acquire(bytes->size()); std::copy(bytes->begin(), bytes->end(), raw.begin());
        auto d = psx::instrument::decode(raw);
        publish({{}, v.origin, psx::instrument::captain_stab_trim, State::frame,
                 d.generation, d.sequence, qpc(), raw}, qpc());
    }
    catch (...) { abort(); throw; }
}
void TrimReceiver::status(std::wstring text, bool connected, bool available)
{
    std::lock_guard lock(mutex_); view_.status = std::move(text);
    if(!connected&&view_.connected){++view_.epoch;pending_=-1;}
    view_.connected = connected; view_.available = available;
    if(!connected||!available)view_.observation_current=false;
}
TrimView TrimReceiver::view()
{
    std::lock_guard lock(mutex_); auto copy = view_;
    if (copy.enabled && copy.frame && copy.received && qpc() - copy.received >= 2 * frequency())
        copy.status = L"STAB TRIM source stale — last indicator retained";
    return copy;
}
void TrimReceiver::decode()
{
    for (;;)
    {
        int slot; uint64_t epoch;
        {
            std::unique_lock lock(mutex_); ready_.wait(lock, [&] { return quit_ || pending_ >= 0; });
            if (quit_) return;
            slot = pending_; pending_ = -1; working_ = slot; epoch = epochs_[slot];
        }
        bool budget{};
        try
        {
            constexpr size_t maximum = 8 * 1024 * 1024;
            if (!tcp::reserve_model(maximum)){budget=true;throw std::runtime_error("Instrument model CPU budget");}
            struct Owner
            {
                size_t bytes{8 * 1024 * 1024}; std::shared_ptr<const psx::du::Frame> frame;
                ~Owner() { frame.reset(); tcp::release_model(bytes); }
            };
            std::shared_ptr<Owner> owner;
            try { owner = std::make_shared<Owner>(); }
            catch (...) { tcp::release_model(maximum); throw; }
            owner->frame = psx::du::decode_instrument(slots_[slot]);
            auto actual = owner->frame->bytes();
            if (actual > maximum) throw std::runtime_error("Instrument decoded model bound");
            tcp::release_model(maximum - actual); owner->bytes = actual;
            auto frame = std::shared_ptr<const psx::du::Frame>(owner, owner->frame.get());
            std::lock_guard lock(mutex_);
            if (!quit_ && view_.enabled && epoch == view_.epoch)
            {
                view_.frame = std::move(frame); view_.origin = origins_[slot];
                view_.received = received_[slot];view_.correlation=correlations_[slot];view_.connection=connections_[slot];view_.observation_current=true; ++view_.accepted;
            }
        }
        catch (const std::exception &)
        {
            std::lock_guard lock(mutex_);if(budget)++view_.budget_rejected;else ++view_.invalid;view_.observation_current=false;
            view_.status = L"STAB TRIM frame rejected — main DU continues";
        }
        std::lock_guard lock(mutex_); working_ = -1;
    }
}
} // namespace du_client
