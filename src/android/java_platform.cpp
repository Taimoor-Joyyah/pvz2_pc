#include <acp/android.hpp>

namespace acp::android {
namespace {
void collections(Runtime &runtime) {
    auto &j = *runtime.jni;
    auto hook = [&runtime, &j](std::string type, std::string name, std::string signature,
                               JavaHandler handler) {
        j.hook(std::move(type), std::move(name), std::move(signature),
               [&runtime, handler = std::move(handler)](JavaCall &call) {
                   std::lock_guard lock(runtime.state_mutex);
                   return handler(call);
               });
    };
    j.set_superclass("java/util/HashMap", "java/util/AbstractMap");
    j.set_superclass("java/util/LinkedHashMap", "java/util/HashMap");
    hook("java/util/HashMap", "<init>", "()V", [](auto &) { return 0; });
    hook("java/util/HashMap", "<init>", "(I)V", [](auto &c) {
        require(static_cast<std::int32_t>(c.arguments[0]) >= 0, "Negative Java HashMap capacity");
        return 0;
    });
    hook("java/util/HashMap", "<init>", "(IF)V", [](auto &c) {
        auto factor = std::bit_cast<float>(static_cast<std::uint32_t>(c.arguments[1]));
        require(static_cast<std::int32_t>(c.arguments[0]) >= 0 && factor > 0.f,
                "Invalid Java HashMap capacity or load factor");
        return 0;
    });
    hook("java/util/HashMap", "<init>", "(Ljava/util/Map;)V", [&j](auto &c) {
        for (auto item : j.get(c.arguments[0]).elements)
            j.get(c.receiver).elements.push_back(j.global_ref(item));
        return 0;
    });
    auto equal = [&j](Address a, Address b) {
        if (!a || !b)
            return a == b;
        const auto &left = j.get(a), &right = j.get(b);
        return &left == &right || (left.class_name == "java/lang/String" &&
                                   right.class_name == left.class_name && left.text == right.text);
    };
    for (const auto *type : {"java/util/HashMap", "java/util/Map"}) {
        hook(type, "size", "()I", [&j](auto &c) { return j.get(c.receiver).elements.size() / 2; });
        hook(type, "isEmpty", "()Z", [&j](auto &c) { return j.get(c.receiver).elements.empty(); });
        hook(type, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", [&j, equal](auto &c) -> Address {
            const auto &items = j.get(c.receiver).elements;
            for (size_t i = 0; i < items.size(); i += 2)
                if (equal(items[i], c.arguments[0]))
                    return j.local_ref(items[i + 1]);
            return 0;
        });
        hook(type, "containsKey", "(Ljava/lang/Object;)Z", [&j, equal](auto &c) {
            const auto &items = j.get(c.receiver).elements;
            for (size_t i = 0; i < items.size(); i += 2)
                if (equal(items[i], c.arguments[0]))
                    return true;
            return false;
        });
        hook(type, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
             [&j, equal](auto &c) -> Address {
                 auto &items = j.get(c.receiver).elements;
                 for (size_t i = 0; i < items.size(); i += 2)
                     if (equal(items[i], c.arguments[0])) {
                         auto previous = j.local_ref(items[i + 1]);
                         j.delete_ref(items[i + 1]);
                         items[i + 1] = j.global_ref(c.arguments[1]);
                         return previous;
                     }
                 items.push_back(j.global_ref(c.arguments[0]));
                 items.push_back(j.global_ref(c.arguments[1]));
                 return 0;
             });
        hook(type, "clear", "()V", [&j](auto &c) {
            auto &items = j.get(c.receiver).elements;
            for (auto value : items)
                j.delete_ref(value);
            items.clear();
            return 0;
        });
        for (bool entries : {false, true})
            hook(type, entries ? "entrySet" : "keySet", "()Ljava/util/Set;", [&j, entries](auto &c) {
                auto out = j.object("java/util/Set");
                const auto &items = j.get(c.receiver).elements;
                for (size_t i = 0; i < items.size(); i += 2) {
                    auto value = items[i];
                    if (entries) {
                        value = j.object("java/util/Map$Entry");
                        j.get(value).elements = {j.global_ref(items[i]), j.global_ref(items[i + 1])};
                    }
                    j.get(out).elements.push_back(j.global_ref(value));
                }
                return out;
            });
    }
    hook("java/util/Map$Entry", "getKey", "()Ljava/lang/Object;",
         [&j](auto &c) { return j.local_ref(j.get(c.receiver).elements.at(0)); });
    hook("java/util/Map$Entry", "getValue", "()Ljava/lang/Object;",
         [&j](auto &c) { return j.local_ref(j.get(c.receiver).elements.at(1)); });
    for (const auto *type : {"java/util/Set", "java/util/Collection", "java/util/ArrayList"}) {
        if (std::string_view(type) != "java/util/ArrayList")
            hook(type, "size", "()I", [&j](auto &c) { return j.get(c.receiver).elements.size(); });
        hook(type, "iterator", "()Ljava/util/Iterator;", [&j](auto &c) {
            auto out = j.object("java/util/Iterator");
            for (auto value : j.get(c.receiver).elements)
                j.get(out).elements.push_back(j.global_ref(value));
            j.get(out).fields["position"] = 0;
            return out;
        });
    }
    hook("java/util/Iterator", "hasNext", "()Z", [&j](auto &c) {
        const auto &object = j.get(c.receiver);
        return object.fields.at("position").template get<size_t>() < object.elements.size();
    });
    hook("java/util/Iterator", "next", "()Ljava/lang/Object;", [&j](auto &c) {
        auto &object = j.get(c.receiver);
        auto position = object.fields.at("position").template get<size_t>();
        require(position < object.elements.size(), "Java iterator is exhausted");
        object.fields["position"] = position + 1;
        return j.local_ref(object.elements[position]);
    });
}
} // namespace
void install_java_platform(Runtime &r) {
    auto &j = *r.jni;
    collections(r);
    j.set_superclass("android/app/Activity", "android/content/Context");
    j.set_superclass("android/app/Application", "android/content/Context");
    j.set_superclass("android/view/SurfaceView", "android/view/View");
    auto audio_manager = j.object("android/media/AudioManager");
    auto application = j.object("android/app/Application");
    j.field("android/content/Context", "AUDIO_SERVICE", "Ljava/lang/String;", j.string("audio"));
    j.field("android/media/AudioManager", "STREAM_MUSIC", "I", 3);
    j.field("android/media/AudioManager", "AUDIOFOCUS_GAIN", "I", 1);
    j.field("android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE", "Ljava/lang/String;",
            j.string("android.media.property.OUTPUT_SAMPLE_RATE"));
    j.field("android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "Ljava/lang/String;",
            j.string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"));
    j.hook("android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
           [&j, audio_manager](auto &c) -> Address {
               return j.text(c.arguments[0]) == "audio" ? j.local_ref(audio_manager) : 0;
           });
    j.hook("android/content/Context", "getApplicationContext", "()Landroid/content/Context;",
           [&j, application](auto &) { return j.local_ref(application); });
    j.hook("android/content/Context", "getPackageName", "()Ljava/lang/String;",
           [&j, &r](auto &) { return j.string(r.options.package); });
    j.hook("android/content/Context", "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;",
           [&j, &r](auto &) {
               std::lock_guard lock(r.state_mutex);
               auto result = j.object("android/content/pm/ApplicationInfo");
               auto &fields = j.get(result).fields;
               fields["android/content/pm/PackageItemInfo#packageNameLjava/lang/String;"] =
                   j.global_ref(j.string(r.options.package));
               fields["android/content/pm/ApplicationInfo#dataDirLjava/lang/String;"] =
                   j.global_ref(j.string("/data/data/" + r.options.package));
               fields["android/content/pm/ApplicationInfo#nativeLibraryDirLjava/lang/String;"] =
                   j.global_ref(j.string("/data/app/" + r.options.package + "/lib"));
               fields["android/content/pm/ApplicationInfo#targetSdkVersionI"] = r.options.target_sdk;
               fields["android/content/pm/ApplicationInfo#flagsI"] = r.options.application_flags;
               return result;
           });
    j.set_superclass("android/content/pm/ApplicationInfo", "android/content/pm/PackageItemInfo");
    j.field("android/content/pm/ApplicationInfo", "FLAG_DEBUGGABLE", "I", 2);
    auto file = [&j](std::string path) {
        auto result = j.object("java/io/File");
        j.get(result).fields["path"] = std::move(path);
        return result;
    };
    j.hook("android/content/Context", "getFilesDir", "()Ljava/io/File;",
           [&r, file](auto &) { return file("/data/data/" + r.options.package + "/files"); });
    j.hook("android/content/Context", "getCacheDir", "()Ljava/io/File;",
           [&r, file](auto &) { return file("/data/data/" + r.options.package + "/cache"); });
    j.hook("android/content/Context", "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;",
           [&r, &j, file](auto &c) {
               auto path = "/sdcard/Android/data/" + r.options.package + "/files";
               if (c.arguments[0])
                   path += "/" + j.text(c.arguments[0]);
               auto resolved = r.path(path, true);
               fs::create_directories(resolved);
               return file(path);
           });
    for (const auto *method : {"getPath", "getAbsolutePath", "toString"})
        j.hook("java/io/File", method, "()Ljava/lang/String;", [&j](auto &c) {
            return j.string(j.get(c.receiver).fields.at("path").template get<std::string>());
        });
    j.hook("java/io/File", "exists", "()Z", [&j, &r](auto &c) {
        auto path = r.resolve_path(j.get(c.receiver).fields.at("path").template get<std::string>());
        return path && fs::exists(*path);
    });
    j.hook("android/media/AudioManager", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;",
           [&j](auto &c) -> Address {
               auto property = j.text(c.arguments[0]);
               if (property == "android.media.property.OUTPUT_SAMPLE_RATE")
                   return j.string("44100");
               if (property == "android.media.property.OUTPUT_FRAMES_PER_BUFFER")
                   return j.string("192");
               return 0;
           });
    auto volume = std::make_shared<int>(15);
    j.hook("android/media/AudioManager", "getStreamMaxVolume", "(I)I", [](auto &) { return 15; });
    j.hook("android/media/AudioManager", "getStreamVolume", "(I)I", [&r, volume](auto &) {
        std::lock_guard lock(r.state_mutex);
        return *volume;
    });
    j.hook("android/media/AudioManager", "setStreamVolume", "(III)V", [&r, volume](auto &c) {
        std::lock_guard lock(r.state_mutex);
        *volume = std::clamp(static_cast<int>(c.arguments[1]), 0, 15);
        r.host->audio_gain(*volume / 15.f);
        return 0;
    });
    j.hook("android/media/AudioManager", "requestAudioFocus",
           "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I", [](auto &) { return 1; });
    j.hook("android/media/AudioManager", "abandonAudioFocus",
           "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I", [](auto &) { return 1; });
    j.hook("android/media/AudioManager", "getMode", "()I", [](auto &) { return 0; });
    // Output uses SDL's default desktop device; no Android-managed Bluetooth or headset route exists.
    for (const auto *method : {"isBluetoothA2dpOn", "isBluetoothScoOn", "isWiredHeadsetOn"})
        j.hook("android/media/AudioManager", method, "()Z", [](auto &) { return false; });
    j.hook("android/media/AudioTrack", "getNativeOutputSampleRate", "(I)I", [](auto &) { return 44100; });
}
} // namespace acp::android
