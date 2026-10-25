#pragma once
#include "motion_control.hpp"
namespace coordinator {
// Operator Stop survives reconnects. Disconnect only suspends automatic startup
// until the next explicit Connect; neither action is a saved configuration gate.
class AutomaticFollow {
    bool stopped_{},disconnected_{};
    std::uint64_t attachment_{},operation_{};
public:
    bool inhibited()const noexcept{return stopped_||disconnected_;}
    void disconnect()noexcept{disconnected_=true;}
    void connect()noexcept{disconnected_=false;operation_=0;}
    void stop()noexcept{stopped_=true;}
    void follow()noexcept{stopped_=disconnected_=false;operation_=0;}
    void reset_binding()noexcept{attachment_=operation_=0;}
    void requested(std::uint64_t id)noexcept{operation_=id;}
    bool eligible(const MotionSnapshot& motion,std::uint64_t attachment)noexcept{
        if(attachment!=attachment_){attachment_=attachment;operation_=0;}
        if(inhibited()||!motion.connected||!motion.identity_available||motion.transport_fault||motion.outstanding)return false;
        if(motion.status_available&&(motion.status.flags&psx::motion::follow_requested))return false;
        if(operation_){
            const auto& ack=motion.last_ack;
            if(ack.operation_id!=operation_)return false;
            if(ack.result==psx::motion::AckResult::accepted)return false;
            // After Applied, wait for a subsequent status before deciding the
            // plugin stopped following. Rejected/cancelled local work may retry.
            if(ack.result==psx::motion::AckResult::applied&&
               (!motion.status_available||motion.status.generated_qpc<ack.generated_qpc))return false;
            operation_=0;
        }
        return true;
    }
};
}
