#include "hwio/catalogue.hpp"
namespace hwio {
const std::vector<ButtonAction> &button_catalogue() { return active_catalogue().actions; }
std::string button_chapter(int n) {
    const auto &chapters=active_catalogue().chapters;
    const auto it=chapters.find(n);
    return it==chapters.end()?"Unavailable assignments":it->second;
}
}
