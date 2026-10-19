#include <acp/android.hpp>

#include <cmath>
namespace acp::android {
struct Audio::Impl {
    Runtime &r;
    std::map<Address, std::string> identifiers;
    std::uint64_t engines_created{}, players_created{}, streams_opened{}, submitted_pcm_bytes{},
        completed_callbacks{};
    std::map<std::string, std::uint64_t> unsupported_requests;
    bool pump_scheduled{};
    void schedule_pump() {
        if (pump_scheduled || r.stopping || !r.options.automatic_audio_callbacks)
            return;
        pump_scheduled = true;
        r.post_callback(
            [this](guest::Cpu &cpu) {
                {
                    std::lock_guard lock(r.state_mutex);
                    pump_scheduled = false;
                }
                r.audio->pump(cpu);
            },
            std::chrono::milliseconds(2));
    }
    struct Player {
        Address object{}, engine{}, play{}, queue{}, configuration{}, volume{}, callback{},
            callback_context{}, object_callback{}, object_context{};
        unsigned state = 1;
        int channels = 2, frequency = 44100;
        struct Buffer {
            Bytes bytes;
            std::uint64_t end{};
            bool submitted{};
        };
        std::deque<Buffer> buffers;
        std::shared_ptr<host::AudioStream> output;
        std::uint64_t submitted{}, completed{};
        std::uint32_t capacity = 16, index{};
        std::uint64_t generation{};
        std::int16_t millibels{};
        bool realized{}, playable{}, muted{};
        void gain() {
            if (output)
                output->gain(muted ? 0.f : std::pow(10.f, millibels / 2000.f));
        }
        void clear() {
            ++generation;
            buffers.clear();
            submitted = completed = index = 0;
            if (output)
                output->clear();
        }
        void update_position() {
            if (output)
                completed = std::max(
                    completed, submitted - std::min<std::uint64_t>(submitted, output->queued_bytes()));
        }
    };
    std::map<Address, std::shared_ptr<Player>> objects;
    Address object_table{}, engine_table{}, play_table{}, queue_table{}, configuration_table{},
        volume_table{};
    explicit Impl(Runtime &runtime) : r(runtime) {}
    Address handle(Address table, std::shared_ptr<Player> player) {
        auto a = r.memory.allocate(8);
        r.memory.store(a, table);
        objects[a] = std::move(player);
        return a;
    }
    Address create(bool engine = false, bool playable = false) {
        engines_created += engine;
        players_created += playable;
        auto p = std::make_shared<Player>();
        p->playable = playable;
        p->object = handle(object_table, p);
        p->play = handle(play_table, p);
        p->queue = handle(queue_table, p);
        p->configuration = handle(configuration_table, p);
        p->volume = handle(volume_table, p);
        if (engine)
            p->engine = handle(engine_table, p);
        return p->object;
    }
    std::shared_ptr<Player> player(Address a) {
        auto it = objects.find(a);
        require(it != objects.end(), "Invalid OpenSL ES object");
        return it->second;
    }
    void slot(Address table, size_t index, std::string name, std::function<std::uint64_t(guest::Cpu &)> fn,
              bool serialized = true) {
        auto a = r.traps.add("OpenSLES/" + name, [this, fn = std::move(fn), serialized](auto &c) {
            std::unique_lock lock(r.state_mutex, std::defer_lock);
            if (serialized)
                lock.lock();
            c.x(0, fn(c));
        });
        r.memory.store(table + index * 8, a);
    }
};
Audio::Audio(Runtime &runtime) : impl_(std::make_unique<Impl>(runtime)) {
    auto *p = impl_.get();
    auto &r = runtime;
    auto &m = r.memory;
    // Android's music stream is scoped to the audio players exposed by this runtime.
    r.jni->hook("android/media/AudioManager", "isMusicActive", "()Z", [p, &r](auto &) {
        std::lock_guard lock(r.state_mutex);
        for (const auto &[address, player] : p->objects)
            if (address == player->object && player->playable && player->state == 3 && player->output &&
                player->output->queued_bytes() > 0)
                return true;
        return false;
    });
    struct Id {
        const char *name;
        std::uint32_t low;
        std::uint16_t mid, hi, seq;
    };
    const Id ids[] = {{"SL_IID_ENGINE", 0x8d97c260, 0xddd4, 0x11db, 0x958f},
                      {"SL_IID_BUFFERQUEUE", 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99},
                      {"SL_IID_PLAY", 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49},
                      {"SL_IID_ANDROIDCONFIGURATION", 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c},
                      {"SL_IID_VOLUME", 0x09e8ede0, 0xddde, 0x11db, 0xb4f6}};
    for (auto id : ids) {
        auto a = m.allocate(16);
        m.store(a, id.low);
        m.store(a + 4, id.mid);
        m.store(a + 6, id.hi);
        m.store(a + 8, id.seq);
        const std::array<std::uint8_t, 6> node{0, 2, 0xa5, 0xd5, 0xc5, 0x1b};
        m.write(a + 10, std::as_bytes(std::span(node)));
        auto slot = m.allocate(8);
        m.store(slot, a);
        r.linker.provide_data(id.name, slot);
        p->identifiers[a] = id.name;
    }
    p->object_table = m.allocate(10 * 8);
    p->engine_table = m.allocate(15 * 8);
    p->play_table = m.allocate(12 * 8);
    p->queue_table = m.allocate(4 * 8);
    p->configuration_table = m.allocate(2 * 8);
    p->volume_table = m.allocate(9 * 8);
    for (auto [table, n] : std::vector<std::pair<Address, size_t>>{{p->object_table, 10},
                                                                   {p->engine_table, 15},
                                                                   {p->play_table, 12},
                                                                   {p->queue_table, 4},
                                                                   {p->configuration_table, 2},
                                                                   {p->volume_table, 9}})
        for (size_t i = 0; i < n; ++i)
            p->slot(table, i, "Unsupported", [p, table, i](auto &) {
                ++p->unsupported_requests[std::format("interface={:#x}, slot={}", table, i)];
                return 12;
            });
    r.bind("slCreateEngine", [p, &m, &r](auto &c) {
        std::lock_guard lock(r.state_mutex);
        m.store(c.x(0), p->create(true));
        return 0;
    });
    p->slot(
        p->object_table, 0, "Realize",
        [p](auto &c) {
            Address callback{}, context{}, object{};
            {
                std::lock_guard lock(p->r.state_mutex);
                auto player = p->player(c.x(0));
                if (c.x(1) > 1)
                    return 2;
                if (c.x(1) && !player->engine) {
                    ++p->unsupported_requests["asynchronous non-engine realization"];
                    return 12;
                }
                if (player->realized)
                    return 1;
                player->realized = true;
                callback = player->object_callback;
                context = player->object_context;
                object = player->object;
            }
            // AOSP IObject.cpp completes asynchronous engine realization synchronously, then notifies.
            if (c.x(1) && callback)
                c.call(callback, std::array<Address, 6>{object, context, 2, 0, 2, 0});
            return 0;
        },
        false);
    p->slot(p->object_table, 4, "RegisterObjectCallback", [p](auto &c) {
        auto player = p->player(c.x(0));
        player->object_callback = c.x(1);
        player->object_context = c.x(2);
        return 0;
    });
    p->slot(p->object_table, 1, "Resume", [p](auto &c) {
        p->player(c.x(0))->realized = true;
        return 0;
    });
    p->slot(p->object_table, 2, "GetState", [p, &m](auto &c) {
        m.store(c.x(1), std::uint32_t(p->player(c.x(0))->realized ? 2 : 1));
        return 0;
    });
    p->slot(p->object_table, 3, "GetInterface", [p, &m](auto &c) {
        auto player = p->player(c.x(0));
        std::string name;
        for (const auto &[address, id] : p->identifiers)
            if (std::ranges::equal(m.readable(c.x(1), 16), m.readable(address, 16)))
                name = id;
        Address out = 0;
        if (name == "SL_IID_ENGINE")
            out = player->engine;
        else if (name == "SL_IID_PLAY")
            out = player->play;
        else if (name == "SL_IID_BUFFERQUEUE")
            out = player->queue;
        else if (name == "SL_IID_ANDROIDCONFIGURATION")
            out = player->configuration;
        else if (name == "SL_IID_VOLUME" && player->playable)
            out = player->volume;
        m.store(c.x(2), out);
        if (!out) {
            if (name.empty()) {
                name = "IID:";
                for (auto byte : m.readable(c.x(1), 16))
                    name += std::format("{:02x}", std::to_integer<unsigned>(byte));
            }
            ++p->unsupported_requests[name];
        }
        return out ? 0 : 12;
    });
    p->slot(p->object_table, 6, "Destroy", [p](auto &c) {
        auto player = p->player(c.x(0));
        player->state = 1;
        player->callback = 0;
        player->clear();
        player->output.reset();
        for (auto it = p->objects.begin(); it != p->objects.end();) {
            if (it->second == player) {
                p->r.memory.release(it->first);
                it = p->objects.erase(it);
            } else
                ++it;
        }
        return 0;
    });
    p->slot(p->engine_table, 7, "CreateOutputMix", [p, &m](auto &c) {
        auto engine = p->player(c.x(0));
        if (engine->engine != c.x(0))
            return 2;
        auto mix = p->create();
        p->player(mix)->engine = engine->engine;
        m.store(c.x(1), mix);
        return 0;
    });
    p->slot(p->engine_table, 2, "CreateAudioPlayer", [p, &m](auto &c) {
        auto engine = p->player(c.x(0));
        if (engine->engine != c.x(0))
            return 2;
        if (c.x(3)) {
            auto locator = m.load<Address>(c.x(3));
            if (locator && m.load<std::uint32_t>(locator) == 3) {
                auto mix = p->objects.find(m.load<Address>(locator + 8));
                if (mix == p->objects.end() || mix->second->engine != engine->engine)
                    return 2;
            }
        }
        auto format = m.load<Address>(c.x(2) + 8);
        if (m.load<std::uint32_t>(format) != 2)
            return 9;
        auto channels = m.load<std::uint32_t>(format + 4),
             frequency = m.load<std::uint32_t>(format + 8) / 1000,
             bits = m.load<std::uint32_t>(format + 12);
        if ((channels != 1 && channels != 2) || bits != 16 || m.load<std::uint32_t>(format + 16) != 16 ||
            m.load<std::uint32_t>(format + 24) != 2 || m.load<std::uint32_t>(format + 8) % 1000 ||
            frequency < 8000 || frequency > 192000)
            return 9;
        auto locator = m.load<Address>(c.x(2));
        auto capacity = m.load<std::uint32_t>(locator + 4);
        if (!capacity || capacity > 256)
            return 2;
        auto object = p->create(false, true);
        auto player = p->player(object);
        player->engine = engine->engine;
        player->channels = static_cast<int>(channels);
        player->frequency = static_cast<int>(frequency);
        player->capacity = capacity;
        m.store(c.x(1), object);
        return 0;
    });
    p->slot(p->play_table, 0, "SetPlayState", [p](auto &c) {
        auto player = p->player(c.x(0));
        if (c.x(1) < 1 || c.x(1) > 3)
            return 2;
        auto state = static_cast<unsigned>(c.x(1));
        if (!player->playable)
            return 1;
        if (state == 3 && !player->output) {
            try {
                player->output = p->r.host->create_audio(player->frequency, player->channels);
                ++p->streams_opened;
                player->gain();
            } catch (const Error &error) {
                player->output.reset();
                p->r.message("Audio device unavailable: " + std::string(error.what()));
                return 4;
            }
        }
        if (player->output) {
            player->output->pause(state != 3);
            if (state == 1) {
                player->output->clear();
                player->submitted = player->completed = 0;
                for (auto &buffer : player->buffers) {
                    buffer.submitted = false;
                    buffer.end = 0;
                }
            }
        }
        player->state = state;
        if (state == 3)
            p->schedule_pump();
        return 0;
    });
    p->slot(p->play_table, 1, "GetPlayState", [p, &m](auto &c) {
        m.store(c.x(1), p->player(c.x(0))->state);
        return 0;
    });
    p->slot(p->play_table, 2, "GetDuration", [&m](auto &c) {
        m.store(c.x(1), std::uint32_t{UINT32_MAX});
        return 0;
    });
    p->slot(p->play_table, 3, "GetPosition", [p, &m](auto &c) {
        auto player = p->player(c.x(0));
        player->update_position();
        m.store(c.x(1), static_cast<std::uint32_t>(player->completed * 1000 /
                                                   (player->frequency * player->channels * 2)));
        return 0;
    });
    p->slot(p->queue_table, 0, "Enqueue", [p, &m](auto &c) {
        auto player = p->player(c.x(0));
        if (player->buffers.size() >= player->capacity)
            return 7;
        if (!c.x(2) || c.x(2) % (player->channels * 2))
            return 2;
        auto b = m.readable(c.x(1), c.x(2));
        player->buffers.push_back({Bytes(b.begin(), b.end())});
        if (player->state == 3)
            p->schedule_pump();
        return 0;
    });
    p->slot(p->queue_table, 1, "Clear", [p](auto &c) {
        auto player = p->player(c.x(0));
        player->clear();
        return 0;
    });
    p->slot(p->queue_table, 2, "GetQueueState", [p, &m](auto &c) {
        auto player = p->player(c.x(0));
        m.store(c.x(1), static_cast<std::uint32_t>(player->buffers.size()));
        m.store(c.x(1) + 4, player->index);
        return 0;
    });
    p->slot(p->queue_table, 3, "RegisterCallback", [p](auto &c) {
        auto player = p->player(c.x(0));
        player->callback = c.x(1);
        player->callback_context = c.x(2);
        return 0;
    });
    p->slot(p->configuration_table, 0, "SetConfiguration",
            [&m](auto &c) { return m.string(c.x(1)) == "androidPlaybackStreamType" ? 0 : 12; });
    p->slot(p->volume_table, 0, "SetVolumeLevel", [p](auto &c) {
        auto value = static_cast<std::int16_t>(c.x(1));
        if (value > 0)
            return 2;
        auto player = p->player(c.x(0));
        player->millibels = value;
        player->gain();
        return 0;
    });
    p->slot(p->volume_table, 1, "GetVolumeLevel", [p, &m](auto &c) {
        m.store(c.x(1), p->player(c.x(0))->millibels);
        return 0;
    });
    p->slot(p->volume_table, 2, "GetMaxVolumeLevel", [&m](auto &c) {
        m.store(c.x(1), std::int16_t{});
        return 0;
    });
    p->slot(p->volume_table, 3, "SetMute", [p](auto &c) {
        if (c.x(1) > 1)
            return 2;
        auto player = p->player(c.x(0));
        player->muted = c.x(1) != 0;
        player->gain();
        return 0;
    });
    p->slot(p->volume_table, 4, "GetMute", [p, &m](auto &c) {
        m.store(c.x(1), std::uint32_t(p->player(c.x(0))->muted));
        return 0;
    });
}
Audio::~Audio() = default;
void Audio::pump(guest::Cpu &cpu) {
    auto &p = *impl_;
    struct Callback {
        std::shared_ptr<Impl::Player> player;
        Address function, context;
        std::uint64_t generation;
    };
    std::vector<Callback> callbacks;
    {
        std::lock_guard lock(p.r.state_mutex);
        bool playing = false;
        for (auto &[address, player] : p.objects) {
            if (address != player->object || player->state != 3 || !player->output)
                continue;
            playing = true;
            player->update_position();
            while (!player->buffers.empty() && player->buffers.front().submitted &&
                   player->buffers.front().end <= player->completed) {
                player->buffers.pop_front();
                ++player->index;
                if (player->callback)
                    callbacks.push_back(
                        {player, player->callback, player->callback_context, player->generation});
            }
            for (auto &buffer : player->buffers) {
                if (buffer.submitted)
                    continue;
                if (player->output->queued_bytes() >
                    static_cast<size_t>(player->frequency * player->channels * 2 / 10))
                    break;
                player->output->submit(buffer.bytes);
                p.submitted_pcm_bytes += buffer.bytes.size();
                player->submitted += buffer.bytes.size();
                buffer.end = player->submitted;
                buffer.submitted = true;
            }
        }
        if (playing)
            p.schedule_pump();
    }
    for (const auto &callback : callbacks) {
        bool alive;
        {
            std::lock_guard lock(p.r.state_mutex);
            alive = callback.player->callback == callback.function &&
                    callback.player->generation == callback.generation &&
                    p.objects.contains(callback.player->object);
        }
        if (alive) {
            cpu.call(callback.function, std::array{callback.player->queue, callback.context});
            std::lock_guard lock(p.r.state_mutex);
            ++p.completed_callbacks;
        }
    }
}
Json Audio::describe() const {
    const auto &p = *impl_;
    std::lock_guard lock(p.r.state_mutex);
    std::uint64_t live{}, playing{}, queued{};
    for (const auto &[address, player] : p.objects)
        if (address == player->object && player->playable) {
            ++live;
            playing += player->state == 3;
            if (player->output)
                queued += player->output->queued_bytes();
        }
    return {{"engines_created", p.engines_created},
            {"players_created", p.players_created},
            {"streams_opened", p.streams_opened},
            {"submitted_pcm_bytes", p.submitted_pcm_bytes},
            {"completed_callbacks", p.completed_callbacks},
            {"live_players", live},
            {"playing_players", playing},
            {"queued_pcm_bytes", queued},
            {"unsupported_requests", p.unsupported_requests}};
}
} // namespace acp::android
