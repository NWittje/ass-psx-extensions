#pragma once
#include <psx/motion/motion.hpp>
#include <array>
namespace psx::xp {
// Installed hash-pinned OBJ wheels, deploy=1, zero tyre compression/steer/rotation.
// Assembly contact centres in OBJ metres; selected ACF attachment transforms are zero.
inline constexpr std::array<motion::Vec3,3> actor_contacts{{
 {0.000001282506493,-3.434987278859563,4.141415190941503},
 {-2.868671825345662,-3.636430241403082,19.73132778933753},
 {2.86869103611509,-3.636430798222584,19.73131753718066}}};
// Map the deployed contact plane directly to horizontal tangent up. The Y row
// of body_rotation is (-sin(bank)*cos(pitch), cos(bank)*cos(pitch), -sin(pitch)).
// Heading does not affect this row. No terrain query or iterative solve.
inline bool flat_actor_attitude(double& pitch,double& bank)noexcept{
 motion::Vec3 normal;
 if(!motion::normalize(motion::cross(actor_contacts[1]-actor_contacts[0],actor_contacts[2]-actor_contacts[0]),normal)||normal.y<=0)return false;
 pitch=-std::asin((std::clamp)(normal.z,-1.0,1.0))*180/motion::pi;
 bank=std::atan2(-normal.x,normal.y)*180/motion::pi;
 return std::isfinite(pitch)&&std::isfinite(bank);
}
}
