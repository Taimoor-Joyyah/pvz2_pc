#pragma once
#include <acp/host.hpp>

#include <chrono>
namespace acp {
// A live document is append-only; each appended batch uses relative frame offsets.
class ReplaySession {
  public:
    ReplaySession(fs::path path, bool live) : path_(std::move(path)), live_(live) {
        reload(0);
    }
    std::vector<host::Event> take(std::uint64_t frame, int width, int height) {
        auto now = std::chrono::steady_clock::now();
        if (live_ && now >= next_poll_) {
            next_poll_ = now + std::chrono::milliseconds(100);
            if (fs::last_write_time(path_) != modified_)
                reload(frame);
        }
        std::vector<host::Event> result;
        for (auto &queue : queues_) {
            auto events = queue.take(frame, width, height);
            delivered_ += events.size();
            result.insert(result.end(), std::make_move_iterator(events.begin()),
                          std::make_move_iterator(events.end()));
        }
        std::erase_if(queues_, [](const auto &queue) { return queue.remaining() == 0; });
        return result;
    }
    Json report() const {
        auto events = audit_;
        std::stable_sort(events.begin(), events.end(), [](const auto &a, const auto &b) {
            return a.at("frame").template get<std::uint64_t>() <
                   b.at("frame").template get<std::uint64_t>();
        });
        return {{"source", utf8(fs::absolute(path_))},     {"live", live_}, {"delivered", delivered_},
                {"remaining", audit_.size() - delivered_}, {"schema", 1},   {"events", std::move(events)}};
    }

  private:
    void reload(std::uint64_t frame) {
        auto modified = fs::last_write_time(path_);
        auto document = read_json(path_);
        require(document.at("schema") == 1 && document.at("events").is_array(), "Invalid replay document");
        auto &events = document.at("events");
        require(events.size() >= previous_.size(), "Live input history cannot be removed");
        for (size_t i = 0; i < previous_.size(); ++i)
            require(events[i] == previous_[i], "Live input history cannot be changed");
        Json batch{{"schema", 1}, {"events", Json::array()}};
        for (size_t i = previous_.size(); i < events.size(); ++i) {
            auto event = events[i];
            if (live_ && !event.contains("frame"))
                event["frame"] = 0;
            batch["events"].push_back(std::move(event));
        }
        // Validate relative frame types and order before adding the origin.
        host::InputReplay validated(batch);
        for (auto &event : batch["events"]) {
            auto offset = event.at("frame").get<std::uint64_t>();
            require(offset <= UINT64_MAX - frame, "Live input frame overflow");
            event["frame"] = frame + offset;
        }
        queues_.emplace_back(batch);
        for (auto &event : batch["events"])
            audit_.push_back(event);
        previous_ = events;
        modified_ = modified;
    }
    fs::path path_;
    bool live_{};
    fs::file_time_type modified_{};
    std::chrono::steady_clock::time_point next_poll_{};
    Json previous_ = Json::array(), audit_ = Json::array();
    std::vector<host::InputReplay> queues_;
    size_t delivered_{};
};
} // namespace acp
