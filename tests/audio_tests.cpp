#include <acp/android.hpp>

#include <cmath>
#include <future>
#include <iostream>
namespace {
using namespace acp;
class Stream final : public host::AudioStream {
  public:
    std::atomic_size_t queued{};
    std::atomic_bool paused = true;
    std::atomic<float> level = 1.f;
    void submit(std::span<const std::byte> bytes) override {
        queued += bytes.size();
    }
    size_t queued_bytes() const override {
        return queued;
    }
    void pause(bool value) override {
        paused = value;
    }
    void clear() override {
        queued = 0;
    }
    void gain(float value) override {
        level = value;
    }
    void consume(size_t bytes) {
        require(!paused && bytes <= queued, "Invalid simulated audio consumption");
        queued -= bytes;
    }
};
class Host final : public host::Services {
  public:
    std::vector<std::shared_ptr<Stream>> streams;
    void open_window(std::string_view, int, int) override {}
    void *native_window() const override {
        return nullptr;
    }
    std::vector<host::Event> poll() override {
        return {};
    }
    void fullscreen() override {}
    void start_text(bool) override {}
    std::shared_ptr<host::AudioStream> create_audio(int frequency, int channels) override {
        require(frequency > 0 && (channels == 1 || channels == 2), "Invalid host audio format");
        auto result = std::make_shared<Stream>();
        streams.push_back(result);
        return result;
    }
    void audio_gain(float) override {}
};
void contracts() {
    auto host = std::make_unique<Host>();
    auto *device = host.get();
    auto root = fs::current_path() / "test-artifacts" / path_from_utf8("audio-" + timestamp());
    android::Runtime r({"org.example.audio", "1", "en_US", 1, root, {}, {}}, std::move(host));
    r.options.automatic_audio_callbacks = false;
    auto &m = r.memory;
    auto manager = r.jni->object("android/media/AudioManager");
    auto env = r.jni->environment(), jni_table = m.load<Address>(env);
    auto music_method = r.cpu.call(m.load<Address>(jni_table + 33 * 8),
                                   std::array{env, r.jni->class_ref("android/media/AudioManager"),
                                              m.string("isMusicActive"), m.string("()Z")});
    auto music_active = [&] {
        return r.cpu.call(m.load<Address>(jni_table + 37 * 8), std::array{env, manager, music_method}) != 0;
    };
    require(!music_active(), "An idle runtime has no active Android music stream");
    auto call = [&](Address object, size_t slot, std::initializer_list<Address> rest = {}) {
        std::vector<Address> args{object};
        args.insert(args.end(), rest);
        return r.cpu.call(m.load<Address>(m.load<Address>(object) + slot * 8), args);
    };
    auto interface = [&](Address object, std::string_view name) {
        auto out = m.allocate(8);
        auto id = m.load<Address>(r.linker.symbol(name));
        require(call(object, 3, {id, out}) == 0, "OpenSL interface resolution");
        return m.load<Address>(out);
    };
    auto out = m.allocate(8);
    require(r.cpu.call(r.linker.symbol("slCreateEngine"), std::array<Address, 6>{out, 0, 0, 0, 0, 0}) == 0,
            "OpenSL engine creation");
    auto engine_object = m.load<Address>(out);
    unsigned realized = 0;
    auto object_callback = r.traps.add("test/audio-object-realized", [&](auto &c) {
        require(c.x(0) == engine_object && c.x(1) == 0x4321 && c.x(2) == 2 && c.x(3) == 0 && c.x(4) == 2 &&
                    c.x(5) == 0,
                "Engine realization marshals all six object callback arguments");
        ++realized;
    });
    require(call(engine_object, 4, {object_callback, 0x4321}) == 0 && call(engine_object, 0, {1}) == 0 &&
                realized == 1,
            "OpenSL engine realization notifies its registered callback");
    auto engine = interface(engine_object, "SL_IID_ENGINE");
    std::vector<Address> extra_engine_objects;
    for (unsigned i = 0; i < 4; ++i) {
        auto extra_out = m.allocate(8);
        require(r.cpu.call(r.linker.symbol("slCreateEngine"),
                           std::array<Address, 6>{extra_out, 0, 0, 0, 0, 0}) == 0,
                "Multiple OpenSL engines can be created without a fixed limit");
        extra_engine_objects.push_back(m.load<Address>(extra_out));
    }
    auto second_engine_object = extra_engine_objects.front();
    auto second_engine = interface(second_engine_object, "SL_IID_ENGINE");
    auto create_output_mix = [&](Address engine_interface) {
        auto mix_out = m.allocate(8);
        require(call(engine_interface, 7, {mix_out}) == 0, "OpenSL output mix creation");
        return m.load<Address>(mix_out);
    };
    auto first_mix = create_output_mix(engine), second_mix = create_output_mix(second_engine);
    auto output_mix_sink = [&](Address mix) {
        auto locator = m.allocate(16);
        m.store(locator, std::uint32_t{3});
        m.store(locator + 8, mix);
        auto sink = m.allocate(16);
        m.store(sink, locator);
        return sink;
    };
    auto first_sink = output_mix_sink(first_mix), second_sink = output_mix_sink(second_mix);
    auto make_source = [&](unsigned frequency, unsigned channels) {
        auto locator = m.allocate(8);
        m.store(locator, std::uint32_t{6});
        m.store(locator + 4, std::uint32_t{2});
        auto format = m.allocate(28);
        std::array<unsigned, 7> fields{2, channels, frequency * 1000, 16, 16, channels == 1 ? 4u : 3u, 2};
        m.write(format, std::as_bytes(std::span(fields)));
        auto source = m.allocate(16);
        m.store(source, locator);
        m.store(source + 8, format);
        return source;
    };
    auto create_player = [&](Address engine_interface, unsigned frequency, unsigned channels,
                             Address sink) {
        auto source = make_source(frequency, channels);
        auto format = m.load<Address>(source + 8);
        auto object = m.allocate(8);
        m.store(format, std::uint32_t{1});
        require(call(engine_interface, 2, {object, source, sink, 0, 0, 0}) == 9,
                "MIME format is not accepted as PCM");
        m.store(format, std::uint32_t{2});
        require(call(engine_interface, 2, {object, source, sink, 0, 0, 0}) == 0,
                "Android PCM format 2 is accepted");
        auto value = m.load<Address>(object);
        call(value, 0, {0});
        return value;
    };
    auto mismatched_source = make_source(22050, 1), mismatched_player = m.allocate(8);
    require(call(engine, 2, {mismatched_player, mismatched_source, second_sink, 0, 0, 0}) == 2,
            "An audio player cannot use another engine's output mix");
    auto object = create_player(engine, 44100, 2, first_sink);
    auto play = interface(object, "SL_IID_PLAY"), queue = interface(object, "SL_IID_BUFFERQUEUE");
    auto volume = interface(object, "SL_IID_VOLUME");
    unsigned callbacks = 0;
    auto callback = r.traps.add("test/audio-buffer-complete", [&](auto &c) {
        require(c.x(0) == queue && c.x(1) == 0x1234, "OpenSL callbacks marshal interface and user context");
        ++callbacks;
    });
    require(call(queue, 3, {callback, 0x1234}) == 0, "Audio callback registration");
    auto bytes = m.allocate(64);
    require(call(queue, 0, {bytes, 16}) == 0 && call(queue, 0, {bytes, 16}) == 0 &&
                call(queue, 0, {bytes, 16}) == 7,
            "OpenSL buffer capacity is enforced");
    require(call(play, 0, {3}) == 0, "Audio starts");
    r.audio->pump(r.cpu);
    require(device->streams.size() == 1 && device->streams[0]->queued == 32 && callbacks == 0,
            "Submitting buffers does not prematurely complete them");
    auto first = device->streams[0];
    require(music_active(), "Queued playing audio activates the Android music stream");
    first->consume(8);
    r.audio->pump(r.cpu);
    require(callbacks == 0, "Partially consumed buffers remain queued");
    first->consume(8);
    r.audio->pump(r.cpu);
    require(callbacks == 1, "Consumed buffers invoke exactly one guest callback");
    auto state = m.allocate(8);
    call(queue, 2, {state});
    require(m.load<unsigned>(state) == 1 && m.load<unsigned>(state + 4) == 1,
            "Queue state retains in-flight buffers and processed index");
    call(play, 0, {2});
    require(first->paused && first->queued == 16, "Pausing retains queued audio");
    require(!music_active(), "Paused audio does not report an active music stream");
    call(play, 0, {3});
    call(play, 0, {3});
    require(!first->paused && device->streams.size() == 1 && first->queued == 16,
            "Resume and repeated play preserve the audio device and samples");
    call(volume, 0, {static_cast<Address>(-2000ll)});
    require(std::abs(first->level - 0.1f) < 0.00001f, "OpenSL millibels convert to amplitude gain");
    call(volume, 3, {1});
    require(first->level == 0.f, "OpenSL mute silences only its stream");
    auto second_object = create_player(second_engine, 22050, 1, second_sink);
    auto second_play = interface(second_object, "SL_IID_PLAY"),
         second_queue = interface(second_object, "SL_IID_BUFFERQUEUE");
    call(second_queue, 0, {bytes, 8});
    call(second_play, 0, {3});
    r.audio->pump(r.cpu);
    require(device->streams.size() == 2 && first->queued == 16 && device->streams[1]->queued == 8,
            "Independent OpenSL players use separate host streams");
    call(queue, 1);
    call(queue, 2, {state});
    require(first->queued == 0 && m.load<unsigned>(state) == 0 && m.load<unsigned>(state + 4) == 0 &&
                device->streams[1]->queued == 8,
            "Clearing resets one player's queue without affecting another");
    call(second_play, 0, {1});
    require(device->streams[1]->paused && device->streams[1]->queued == 0,
            "Stopping pauses and clears host output");
    require(!music_active(), "Stopped or empty players do not report active music");
    r.options.automatic_audio_callbacks = true;
    auto background_object = create_player(engine, 44100, 1, first_sink);
    auto background_play = interface(background_object, "SL_IID_PLAY"),
         background_queue = interface(background_object, "SL_IID_BUFFERQUEUE");
    std::promise<void> background_completed;
    auto completion = background_completed.get_future();
    auto background_callback = r.traps.add("test/background-audio-complete", [&](auto &cpu) {
        require(cpu.tls() != r.cpu.tls() && cpu.x(0) == background_queue,
                "Audio callbacks execute on their managed guest worker");
        background_completed.set_value();
    });
    call(background_queue, 3, {background_callback, 0});
    call(background_queue, 0, {bytes, 16});
    call(background_play, 0, {3});
    auto background_stream = device->streams.back();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (background_stream->queued != 16 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(background_stream->queued == 16, "Playing buffers reach the host without a frame-loop poll");
    background_stream->consume(16);
    require(completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "Consumed audio completes while the render thread is idle");
    call(background_play, 0, {1});
    r.wait_callbacks();
    call(background_object, 6);
    call(object, 6);
    call(second_object, 6);
    call(first_mix, 6);
    call(second_mix, 6);
    call(engine_object, 6);
    for (auto extra_engine_object : extra_engine_objects)
        call(extra_engine_object, 6);
    auto report = r.audio->describe();
    require(report.at("engines_created") == 5 && report.at("players_created") == 3 &&
                report.at("streams_opened") == 3 &&
                report.at("submitted_pcm_bytes") == 56 && report.at("completed_callbacks") == 2 &&
                report.at("live_players") == 0,
            "Audio diagnostics retain playback evidence after player destruction");
}
} // namespace
int main() {
    try {
        contracts();
        std::cout << "OpenSL audio contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
