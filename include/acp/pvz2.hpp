#pragma once
#include <acp/android.hpp>
namespace acp::games {
class Pvz2Input {
  public:
    void push(const host::Event &event);
    bool drain(std::span<std::byte> buffer);

  private:
    std::deque<Bytes> events_;
    bool pressed_{};
    int previous_x_{}, previous_y_{};
};
struct Pvz2Options {
    int render_width{}, render_height{}, fps_limit{};
};
std::unique_ptr<android::GameAdapter> pvz2(Pvz2Options options = {});
void install_pvz2_play(android::Runtime &runtime);
Address install_pvz2_billing(android::Runtime &runtime,
                             std::function<void(std::string, Address)> broadcast);
void install_pvz2_persistence(android::Runtime &runtime);
std::map<std::string, Address> install_pvz2_environment(android::Runtime &runtime, Address activity);
std::function<void(bool)> install_pvz2_glu(android::Runtime &runtime);
void install_pvz2_analytics(android::Runtime &runtime);
void install_pvz2_ads(android::Runtime &runtime);
void install_pvz2_http(android::Runtime &runtime);
} // namespace acp::games
