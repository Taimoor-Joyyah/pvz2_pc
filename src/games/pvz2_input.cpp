#include <acp/pvz2.hpp>
namespace acp::games {
namespace {
template <class T> void append(Bytes &bytes, T value) {
    auto offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
double event_time() {
    // AndroidUIEventManager.GetTimeStamp divides System.nanoTime() by 1e9.
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace
void Pvz2Input::push(const host::Event &event) {
    Bytes record;
    auto touch = [&](int phase, int x, int y) {
        append(record, 0u);
        // The Java shell reserves zero and serializes Android pointer ID + 1.
        append(record, 1u);
        append(record, x);
        append(record, y);
        append(record, previous_x_);
        append(record, previous_y_);
        append(record, 1u);
        append(record, event_time());
        append(record, phase);
        append(record, 0xdeadbeefu);
        append(record, 0xdeadbeefu);
        previous_x_ = x;
        previous_y_ = y;
    };
    switch (event.type) {
    case host::EventType::PointerDown:
        previous_x_ = static_cast<int>(event.x);
        previous_y_ = static_cast<int>(event.y);
        pressed_ = true;
        touch(0, previous_x_, previous_y_);
        break;
    case host::EventType::PointerMove:
        if (pressed_) {
            auto x = static_cast<int>(event.x), y = static_cast<int>(event.y);
            touch(x == previous_x_ && y == previous_y_ ? 2 : 1, x, y);
        }
        break;
    case host::EventType::PointerUp:
        if (pressed_)
            touch(3, static_cast<int>(event.x), static_cast<int>(event.y));
        pressed_ = false;
        break;
    case host::EventType::Focus:
        if (!event.focused && pressed_) {
            touch(4, previous_x_, previous_y_);
            pressed_ = false;
        }
        break;
    case host::EventType::Back:
        append(record, 5u);
        for (int i = 0; i < 3; ++i)
            append(record, 0xdeadbeefu);
        break;
    case host::EventType::Key:
        append(record, 1u);
        append(record, event.key_code);
        append(record, event.key_code == 66 ? 13u : 0u);
        append(record, event.released ? 1u : 0u);
        append(record, event_time());
        append(record, 0u);
        append(record, 0xdeadbeefu);
        break;
    case host::EventType::Text: {
        bool backspace = event.text == "\b";
        append(record, 6u);
        append(record, backspace ? 3u : 0u);
        append(record, backspace ? 0u : static_cast<unsigned>(event.text.size()));
        if (!backspace) {
            auto b = std::as_bytes(std::span(event.text));
            record.insert(record.end(), b.begin(), b.end());
            record.resize(static_cast<size_t>(align_up(record.size(), 4)));
        }
        break;
    }
    default:
        break;
    }
    if (!record.empty()) {
        require(events_.size() < 4096, "PVZ2 input queue overflow");
        events_.push_back(std::move(record));
    }
}
bool Pvz2Input::drain(std::span<std::byte> buffer) {
    require(buffer.size() >= 16, "PVZ2 event buffer is smaller than its header");
    size_t offset = 16;
    std::uint32_t count = 0;
    while (!events_.empty() && events_.front().size() <= buffer.size() - offset) {
        const auto &next = events_.front();
        std::memcpy(buffer.data() + offset, next.data(), next.size());
        offset += next.size();
        events_.pop_front();
        ++count;
    }
    std::memcpy(buffer.data(), &count, 4);
    return !events_.empty();
}
} // namespace acp::games
