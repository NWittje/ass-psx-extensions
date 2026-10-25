#pragma once
#include <Windows.h>
#include <string_view>
#include <string>
#include <psx/hwio/motors.hpp>
#include <psx/hwio/actuators.hpp>
#include <psx/hwio/thrust_control.hpp>
namespace hwio {
template<class Packet>class MotorPipeChannel {
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    OVERLAPPED io_{};
    enum class Phase {closed,connect,read,reply,write} phase_{Phase::closed};
    Packet packet_{};
    bool pending_{},lost_{};
    DWORD error_{};
    bool complete(DWORD&);
    void read();
    void reset();
  public:
    MotorPipeChannel();
    ~MotorPipeChannel();
    bool open(std::wstring_view suffix={});
    void close();
    const Packet* poll();
    void reply(const Packet&);
    bool take_loss() noexcept {const bool r=lost_;lost_=false;return r;}
    HANDLE event() const noexcept {return io_.hEvent;}
    DWORD error() const noexcept {return error_;}
    psx::hwio::thrust_control::Process client()const noexcept;
};
using MotorPipe=MotorPipeChannel<psx::hwio::motors::Packet>;
using MotorPipeV2=MotorPipeChannel<psx::hwio::motors::v2::Packet>;
using ActuatorPipe=MotorPipeChannel<psx::hwio::actuators::Packet>;
using ThrustPipe=MotorPipeChannel<psx::hwio::thrust_control::Packet>;
}
