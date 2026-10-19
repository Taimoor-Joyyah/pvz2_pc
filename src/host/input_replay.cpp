#include <acp/host.hpp>

#include <cmath>
namespace acp::host {
InputReplay::InputReplay(const Json &document) {
    require(document.at("schema") == 1 && document.at("events").is_array(),
            "Input replay requires schema 1 and an events array");
    for (const auto &item : document.at("events")) {
        const auto &number = item.at("frame");
        require(number.is_number_unsigned() ||
                    (number.is_number_integer() && number.get<std::int64_t>() >= 0),
                "Replay frames must be nonnegative integers");
        auto frame = number.get<std::uint64_t>();
        require(events_.empty() || frame >= events_.back().first, "Replay events must be ordered by frame");
        auto type = item.at("type").get<std::string>();
        Event event{};
        if (type == "down" || type == "move" || type == "up") {
            event.type = type == "down" ? EventType::PointerDown
                         : type == "up" ? EventType::PointerUp
                                        : EventType::PointerMove;
            event.x = item.at("x").get<float>();
            event.y = item.at("y").get<float>();
            require(std::isfinite(event.x) && std::isfinite(event.y) && event.x >= 0 && event.x <= 1 &&
                        event.y >= 0 && event.y <= 1,
                    "Replay pointer coordinates must be in [0, 1]");
        } else if (type == "back")
            event.type = EventType::Back;
        else if (type == "quit")
            event.type = EventType::Quit;
        else if (type == "text") {
            event.type = EventType::Text;
            event.text = item.at("text").get<std::string>();
        } else if (type == "focus") {
            event.type = EventType::Focus;
            event.focused = item.at("focused").get<bool>();
        } else if (type == "key") {
            event.type = EventType::Key;
            event.key_code = item.at("key_code").get<int>();
            require(event.key_code >= 0 && event.key_code <= 288, "Invalid Android replay key code");
            event.released = item.value("released", false);
        } else
            throw Error("Unknown input replay event: " + type);
        events_.emplace_back(frame, std::move(event));
    }
}
std::vector<Event> InputReplay::take(std::uint64_t frame, int width, int height) {
    require(width > 0 && height > 0, "Replay requires a positive drawable size");
    std::vector<Event> result;
    while (next_ < events_.size() && events_[next_].first <= frame) {
        auto event = events_[next_++].second;
        if (event.type == EventType::PointerDown || event.type == EventType::PointerMove ||
            event.type == EventType::PointerUp) {
            event.x = std::min(event.x * width, float(width - 1));
            event.y = std::min(event.y * height, float(height - 1));
        }
        result.push_back(std::move(event));
    }
    return result;
}
} // namespace acp::host
