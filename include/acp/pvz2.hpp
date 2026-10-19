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
enum class PurchaseMode { Disabled, AutoComplete };
struct Pvz2Options {
    PurchaseMode purchases = PurchaseMode::Disabled;
    Json catalog = Json::array();
    int render_width{}, render_height{}, fps_limit{};
};
Pvz2Options read_pvz2_options(const fs::path &file, bool create = true);
std::unique_ptr<android::GameAdapter> pvz2(Pvz2Options options = {});
void install_pvz2_play(android::Runtime &runtime);
Address install_pvz2_billing(android::Runtime &runtime, const Pvz2Options &options,
                             std::function<void(std::string, Address)> broadcast);
void install_pvz2_persistence(android::Runtime &runtime);
std::map<std::string, Address> install_pvz2_environment(android::Runtime &runtime, Address activity);
std::function<void(bool)> install_pvz2_glu(android::Runtime &runtime);
void install_pvz2_analytics(android::Runtime &runtime);
void install_pvz2_ads(android::Runtime &runtime);
void install_pvz2_http(android::Runtime &runtime);
} // namespace acp::games
