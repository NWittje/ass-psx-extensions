#pragma once
#include <psx/non_normal/protocol.hpp>
#include "motion_support.hpp"
#include "triple_buffer.hpp"
#include <atomic>
#include <thread>
#include <string>
namespace psx::xp {
namespace nn=psx::non_normal;
struct ActorCommand {nn::Request request{};std::uint64_t connection{};};
class RunwayActorService final {
public:
 ~RunwayActorService(){stop();}
 bool start(const std::wstring& endpoint={});void stop()noexcept;
 std::uint64_t connection()const noexcept{return connected_.load(std::memory_order_acquire)?connection_.load(std::memory_order_acquire):0;}
 bool command(ActorCommand& c)noexcept{return commands_.pop(c);}
 void reply(const nn::Result& r,std::uint64_t connection)noexcept;
private:
 struct Reply {nn::Result result{};std::uint64_t connection{};};
 void run()noexcept;
 SpscFifo<ActorCommand,16> commands_;SpscFifo<Reply,32> replies_;
 std::atomic<bool> stop_{},connected_{},overflow_{};
 std::atomic<std::uint64_t> connection_{};std::thread worker_;std::wstring endpoint_;
};
}
