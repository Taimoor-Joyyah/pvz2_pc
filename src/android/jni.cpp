#include <acp/android.hpp>
namespace acp::android {
namespace {
thread_local std::map<const Jni *, Address> active_jni_threads;
struct ThreadContext {
    const Jni *jni;
    Address previous;
    ThreadContext(const Jni *value, Address thread) : jni(value), previous(active_jni_threads[value]) {
        active_jni_threads[value] = thread;
    }
    ~ThreadContext() {
        if (previous)
            active_jni_threads[jni] = previous;
        else
            active_jni_threads.erase(jni);
    }
};
std::string key(std::string_view type, std::string_view name, std::string_view signature) {
    return std::string(type) + "#" + std::string(name) + std::string(signature);
}
std::string normalized(std::string name) {
    if (name.starts_with('L') && name.ends_with(';'))
        name = name.substr(1, name.size() - 2);
    std::replace(name.begin(), name.end(), '.', '/');
    return name;
}
std::string jni_escape(std::string_view text) {
    std::string result;
    for (auto unit : widen(text)) {
        if ((unit >= L'a' && unit <= L'z') || (unit >= L'A' && unit <= L'Z') ||
            (unit >= L'0' && unit <= L'9'))
            result += static_cast<char>(unit);
        else if (unit == L'/')
            result += '_';
        else if (unit == L'_')
            result += "_1";
        else if (unit == L';')
            result += "_2";
        else if (unit == L'[')
            result += "_3";
        else
            result += std::format("_0{:04x}", static_cast<unsigned>(unit));
    }
    return result;
}
std::vector<char> argument_types(std::string_view signature) {
    require(!signature.empty() && signature.front() == '(', "Invalid JNI method signature");
    std::vector<char> result;
    for (size_t i = 1; i < signature.size() && signature[i] != ')';) {
        char c = signature[i++];
        if (c == '[') {
            result.push_back('L');
            while (i < signature.size() && signature[i] == '[')
                ++i;
            require(i < signature.size(), "Invalid JNI array signature");
            c = signature[i++];
            if (c == 'L') {
                auto end = signature.find(';', i);
                require(end != signature.npos, "Invalid JNI array type");
                i = end + 1;
            }
        } else if (c == 'L') {
            result.push_back(c);
            auto end = signature.find(';', i);
            require(end != signature.npos, "Invalid JNI object type");
            i = end + 1;
        } else {
            require(std::string_view("ZBCSIJFD").find(c) != std::string_view::npos,
                    "Invalid JNI primitive type");
            result.push_back(c);
        }
    }
    require(signature.find(')') != signature.npos, "Invalid JNI method signature");
    return result;
}
std::string modified_utf8(const std::u16string &text) {
    std::string out;
    for (auto c : text) {
        if (c && c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}
std::u16string decode_modified(std::string_view text) {
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        auto b = static_cast<unsigned char>(text[i++]);
        unsigned c;
        if (b < 128)
            c = b;
        else if ((b & 0xe0) == 0xc0) {
            require(i < text.size(), "Truncated JNI UTF8");
            auto d = static_cast<unsigned char>(text[i++]);
            require((d & 0xc0) == 0x80, "Invalid JNI UTF8");
            c = ((b & 31) << 6) | (d & 63);
        } else {
            require((b & 0xf0) == 0xe0 && i + 1 < text.size(), "Invalid modified JNI UTF8");
            auto d = static_cast<unsigned char>(text[i++]), e = static_cast<unsigned char>(text[i++]);
            require((d & 0xc0) == 0x80 && (e & 0xc0) == 0x80, "Invalid JNI UTF8");
            c = ((b & 15) << 12) | ((d & 63) << 6) | (e & 63);
        }
        out += static_cast<char16_t>(c);
    }
    return out;
}
} // namespace
Address Jni::reference(std::shared_ptr<Object> value, bool local) {
    std::lock_guard lock(runtime_.state_mutex);
    auto handle = runtime_.memory.allocate(8);
    objects_[handle] = std::move(value);
    auto &frames = frames_[current_thread()];
    if (local && !frames.empty())
        frames.back().push_back(handle);
    return handle;
}
Address Jni::local_ref(Address ref) {
    std::lock_guard lock(runtime_.state_mutex);
    if (!ref)
        return 0;
    return reference(objects_.at(ref));
}
Address Jni::global_ref(Address ref) {
    std::lock_guard lock(runtime_.state_mutex);
    if (!ref)
        return 0;
    return reference(objects_.at(ref), false);
}
void Jni::delete_ref(Address ref) {
    std::lock_guard lock(runtime_.state_mutex);
    auto node = objects_.extract(ref);
    if (!node.empty()) {
        auto value = std::move(node.mapped());
        for (auto &[_, frames] : frames_)
            for (auto &frame : frames)
                std::erase(frame, ref);
        runtime_.memory.release(ref);
        if (!destroying_ && value.use_count() == 1)
            for (auto element : value->elements)
                delete_ref(element);
    }
}
Address Jni::object(std::string name) {
    auto value = make_object();
    value->class_name = normalized(std::move(name));
    return reference(std::move(value));
}
Address Jni::string(std::string_view value) {
    auto ref = object("java/lang/String");
    auto wide = widen(value);
    get(ref).text.assign(reinterpret_cast<const char16_t *>(wide.data()), wide.size());
    return ref;
}
std::string Jni::text(Address ref) const {
    if (!ref)
        return {};
    const auto &s = get(ref).text;
    return narrow(std::wstring_view(reinterpret_cast<const wchar_t *>(s.data()), s.size()));
}
Object &Jni::get(Address ref) {
    std::lock_guard lock(runtime_.state_mutex);
    auto it = objects_.find(ref);
    require(it != objects_.end(), std::format("Invalid JNI reference {:#x}", ref));
    return *it->second;
}
const Object &Jni::get(Address ref) const {
    std::lock_guard lock(runtime_.state_mutex);
    auto it = objects_.find(ref);
    require(it != objects_.end(), std::format("Invalid JNI reference {:#x}", ref));
    return *it->second;
}
Address Jni::class_ref(std::string name) {
    name = normalized(std::move(name));
    std::lock_guard lock(runtime_.state_mutex);
    auto it = classes_.find(name);
    if (it != classes_.end())
        return reference(objects_.at(it->second));
    auto value = make_object();
    value->class_name = "java/lang/Class";
    value->fields["name"] = name;
    auto cache = runtime_.memory.allocate(8);
    objects_[cache] = value;
    classes_[name] = cache;
    return reference(value);
}
void Jni::hook(std::string type, std::string name, std::string signature, JavaHandler fn) {
    std::lock_guard lock(runtime_.state_mutex);
    hooks_[key(normalized(type), name, signature)] = std::move(fn);
}
void Jni::hook_suppressed(std::string type, std::string name, std::string signature,
                          std::string category, std::string behavior, JavaHandler fn) {
    auto key = type + "." + name + signature;
    runtime_.record_suppression(key, std::move(category), std::move(behavior));
    hook(std::move(type), std::move(name), std::move(signature),
         [key = std::move(key), fn = std::move(fn)](JavaCall &call) {
             call.runtime.suppressed_call(key);
             return fn(call);
         });
}
void Jni::field(std::string type, std::string name, std::string signature, std::uint64_t value) {
    std::lock_guard lock(runtime_.state_mutex);
    if (value && (signature.starts_with('L') || signature.starts_with('[')))
        value = reference(objects_.at(value), false);
    fields_[key(normalized(type), name, signature)] = value;
}
void Jni::load_dex_constants() {
    if (dex_constants_loaded_)
        return;
    dex_constants_loaded_ = true;
    if (!fs::is_directory(runtime_.options.apks))
        return;
    for (const auto &file : fs::directory_iterator(runtime_.options.apks)) {
        if (!file.is_regular_file() || file.path().extension() != ".apk")
            continue;
        Archive archive(file.path());
        for (const auto &name : archive.names()) {
            if (!name.starts_with("classes") || !name.ends_with(".dex") || name.find('/') != name.npos)
                continue;
            for (const auto &constant : dex_constants(archive.read(name))) {
                auto id = key(normalized(constant.class_name), constant.name, constant.signature);
                if (fields_.contains(id))
                    continue;
                if (constant.value.is_string()) {
                    require(constant.signature == "Ljava/lang/String;",
                            "DEX constant string has an incompatible field type");
                    auto literal = object("java/lang/String");
                    get(literal).text = decode_modified(constant.value.get<std::string>());
                    fields_[id] = global_ref(literal);
                } else {
                    require(constant.signature != "Ljava/lang/String;",
                            "DEX constant number has an incompatible field type");
                    fields_[id] = constant.value.get<std::uint64_t>();
                }
            }
        }
    }
}
Address Jni::method(std::string type, std::string name, std::string signature, bool is_static) {
    std::lock_guard lock(runtime_.state_mutex);
    auto ref = runtime_.memory.allocate(8);
    methods_[ref] = {std::move(type), std::move(name), std::move(signature), is_static};
    return ref;
}
void Jni::register_native(std::string type, std::string name, std::string signature, Address target) {
    std::lock_guard lock(runtime_.state_mutex);
    require(runtime_.memory.valid(target, 4, guest::Execute), "JNI registration target is not executable");
    natives_[key(normalized(type), name, signature)] = target;
}
Address Jni::native(std::string_view type, std::string_view name, std::string_view signature) const {
    std::lock_guard lock(runtime_.state_mutex);
    auto normalized_type = normalized(std::string(type));
    auto it = natives_.find(key(normalized_type, name, signature));
    if (it != natives_.end())
        return it->second;
    auto symbol = "Java_" + jni_escape(normalized_type) + "_" + jni_escape(name);
    if (auto address = runtime_.linker.symbol(symbol))
        return address;
    auto end = signature.find(')');
    require(signature.starts_with('(') && end != signature.npos, "Invalid native JNI signature");
    return runtime_.linker.symbol(symbol + "__" + jni_escape(signature.substr(1, end - 1)));
}
Address Jni::environment(guest::Cpu &cpu) {
    ThreadContext context(this, cpu.tls());
    std::lock_guard lock(runtime_.state_mutex);
    auto &env = environments_[cpu.tls()];
    if (!env) {
        env = runtime_.memory.allocate(8);
        runtime_.memory.store(env, runtime_.memory.load<Address>(environment_));
        push_frame();
    }
    return env;
}
Json Jni::describe() const {
    std::lock_guard lock(runtime_.state_mutex);
    Json names = Json::array();
    for (const auto &[name, _] : natives_)
        names.push_back(name);
    return {{"registered_natives", names}, {"live_references", objects_.size()}, {"calls", calls_}};
}
Address Jni::current_thread() const {
    auto it = active_jni_threads.find(this);
    return it == active_jni_threads.end() ? runtime_.cpu.tls() : it->second;
}
void Jni::push_frame() {
    std::lock_guard lock(runtime_.state_mutex);
    frames_[current_thread()].emplace_back();
}
Address Jni::pop_frame(Address preserve) {
    std::lock_guard lock(runtime_.state_mutex);
    auto result = preserve ? objects_.at(preserve) : nullptr;
    auto &frames = frames_[current_thread()];
    require(!frames.empty(), "JNI local frame underflow");
    auto expired = std::move(frames.back());
    frames.pop_back();
    for (auto handle : expired)
        delete_ref(handle);
    return result ? reference(result) : 0;
}
std::uint64_t Jni::call_native(guest::Cpu &cpu, Address function, std::span<const Address> arguments,
                               std::uint64_t budget, std::chrono::milliseconds timeout) {
    return with_local_frame(cpu, [&] { return cpu.call(function, arguments, budget, timeout); });
}
std::uint64_t Jni::with_local_frame(guest::Cpu &cpu, const std::function<std::uint64_t()> &work) {
    ThreadContext context(this, cpu.tls());
    size_t depth;
    {
        std::lock_guard lock(runtime_.state_mutex);
        depth = frames_[current_thread()].size();
        push_frame();
    }
    auto cleanup = [&] {
        std::lock_guard lock(runtime_.state_mutex);
        while (frames_[current_thread()].size() > depth)
            pop_frame();
    };
    try {
        auto result = work();
        cleanup();
        return result;
    } catch (...) {
        cleanup();
        throw;
    }
}
std::string Jni::parent(std::string_view child) const {
    if (child == "java/lang/Object")
        return {};
    auto it = parents_.find(std::string(child));
    return it == parents_.end() ? "java/lang/Object" : it->second;
}
bool Jni::assignable(std::string child, std::string_view ancestor) const {
    if (child == ancestor)
        return true;
    if (child.starts_with('[')) {
        if (ancestor == "java/lang/Object" || ancestor == "java/lang/Cloneable" ||
            ancestor == "java/io/Serializable")
            return true;
        return ancestor.starts_with('[') &&
               assignable(normalized(child.substr(1)), normalized(std::string(ancestor.substr(1))));
    }
    if (child.size() == 1 && std::string_view("ZBCSIJFD").find(child[0]) != std::string_view::npos)
        return false;
    std::vector<std::string> pending{std::move(child)};
    std::set<std::string> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (current == ancestor)
            return true;
        if (current.empty() || !visited.insert(current).second)
            continue;
        pending.push_back(parent(current));
        if (auto it = interfaces_.find(current); it != interfaces_.end())
            pending.insert(pending.end(), it->second.begin(), it->second.end());
    }
    return false;
}
void Jni::set_superclass(std::string child, std::string super) {
    std::lock_guard lock(runtime_.state_mutex);
    child = normalized(std::move(child));
    super = normalized(std::move(super));
    require(!assignable(super, child), "Cyclic Java class hierarchy");
    parents_[std::move(child)] = std::move(super);
}
void Jni::set_interfaces(std::string type, std::vector<std::string> interfaces) {
    std::lock_guard lock(runtime_.state_mutex);
    type = normalized(std::move(type));
    for (auto &interface : interfaces) {
        interface = normalized(std::move(interface));
        require(interface != type && !assignable(interface, type), "Cyclic Java interface hierarchy");
    }
    interfaces_[std::move(type)] = std::move(interfaces);
}

std::uint64_t Jni::invoke(guest::Cpu &cpu, bool is_static, int variant, size_t result_kind,
                          bool nonvirtual) {
    auto &memory = runtime_.memory;
    auto mid = cpu.x(nonvirtual ? 3 : 2);
    Method method;
    {
        std::lock_guard lock(runtime_.state_mutex);
        auto it = methods_.find(mid);
        require(it != methods_.end(), "Invalid JNI method ID");
        method = it->second;
    }
    auto types = argument_types(method.signature);
    std::vector<std::uint64_t> args;
    size_t gi = nonvirtual ? 4 : 3, fi = 0;
    Address va = variant == 1 ? cpu.argument(gi) : 0, array = variant == 2 ? cpu.argument(gi) : 0;
    for (size_t i = 0; i < types.size(); ++i) {
        auto type = types[i];
        bool fp = type == 'F' || type == 'D';
        std::uint64_t value{};
        if (variant == 2)
            value = memory.load<std::uint64_t>(array + i * 8);
        else if (variant == 1) {
            auto off = memory.load<std::int32_t>(va + (fp ? 28 : 24));
            Address p;
            if (off < 0) {
                p = memory.load<Address>(va + (fp ? 16 : 8)) + static_cast<std::int64_t>(off);
                memory.store(va + (fp ? 28 : 24), off + (fp ? 16 : 8));
            } else {
                p = memory.load<Address>(va);
                memory.store(va, p + 8);
            }
            value = memory.load<std::uint64_t>(p);
            if (type == 'F')
                value = std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<double>(value)));
        } else if (fp) {
            auto real = cpu.f64(fi++);
            value = type == 'F' ? std::bit_cast<std::uint32_t>(static_cast<float>(real))
                                : std::bit_cast<std::uint64_t>(real);
        } else
            value = cpu.argument(gi++);
        args.push_back(value);
    }
    auto name = key(method.class_name, method.name, method.signature);
    JavaHandler handler;
    {
        std::lock_guard lock(runtime_.state_mutex);
        ++calls_[name];
        auto find = [&](std::string type) {
            for (; !type.empty(); type = parent(type)) {
                auto hook = hooks_.find(key(type, method.name, method.signature));
                if (hook != hooks_.end()) {
                    handler = hook->second;
                    break;
                }
                if (method.name == "<init>")
                    break;
            }
        };
        if (!is_static && !nonvirtual && cpu.x(1))
            find(get(cpu.x(1)).class_name);
        if (!handler)
            find(method.class_name);
    }
    require(bool(handler), "Unimplemented Java method: " + name);
    JavaCall call{runtime_,       cpu, cpu.x(1), method.class_name, method.name, method.signature,
                  std::move(args)};
    auto result = handler(call);
    if (result_kind == 7)
        cpu.f32(0, std::bit_cast<float>(static_cast<std::uint32_t>(result)));
    if (result_kind == 8)
        cpu.f64(0, std::bit_cast<double>(result));
    return result;
}
std::uint64_t Jni::field_access(guest::Cpu &cpu, bool is_static, bool write, size_t result_kind) {
    std::lock_guard lock(runtime_.state_mutex);
    auto it = methods_.find(cpu.x(2));
    require(it != methods_.end(), "Invalid JNI field ID");
    const auto &f = it->second;
    auto name = key(f.class_name, f.name, f.signature);
    if (write) {
        auto value = result_kind == 7   ? std::uint64_t(std::bit_cast<std::uint32_t>(cpu.f32(0)))
                     : result_kind == 8 ? std::bit_cast<std::uint64_t>(cpu.f64(0))
                                        : cpu.x(3);
        if (result_kind == 0 && value)
            value = reference(objects_.at(value), false);
        if (is_static)
            fields_[name] = value;
        else
            get(cpu.x(1)).fields[name] = value;
        return 0;
    }
    std::uint64_t value{};
    bool instance_value = false;
    if (!is_static) {
        const auto &fields = get(cpu.x(1)).fields;
        for (auto type = f.class_name; !type.empty(); type = parent(type)) {
            auto found = fields.find(key(type, f.name, f.signature));
            if (found != fields.end()) {
                value = found->get<std::uint64_t>();
                instance_value = true;
                break;
            }
        }
    }
    if (!instance_value) {
        auto found = fields_.find(name);
        if (is_static && found == fields_.end()) {
            load_dex_constants();
            found = fields_.find(name);
        }
        for (auto type = parent(f.class_name); found == fields_.end() && !type.empty(); type = parent(type))
            found = fields_.find(key(type, f.name, f.signature));
        require(found != fields_.end(), "Unimplemented Java field: " + name);
        value = found->second;
    }
    if (result_kind == 7)
        cpu.f32(0, std::bit_cast<float>(static_cast<std::uint32_t>(value)));
    if (result_kind == 8)
        cpu.f64(0, std::bit_cast<double>(value));
    return result_kind == 0 ? local_ref(value) : value;
}

Jni::Jni(Runtime &runtime) : runtime_(runtime) {
    auto &m = runtime.memory;
    auto table = m.allocate(236 * 8);
    environment_ = m.allocate(8);
    m.store(environment_, table);
    auto slot = [&](size_t i, std::string name, auto fn) {
        auto address = runtime.traps.add("JNI/" + name, [this, i, fn = std::move(fn)](guest::Cpu &c) {
            ThreadContext context(this, c.tls());
            const bool callback = (i >= 28 && i <= 30) || (i >= 34 && i <= 93) || (i >= 114 && i <= 143) ||
                                  i == 217 || i == 218;
            if (callback)
                c.x(0, fn(c));
            else {
                std::lock_guard lock(runtime_.state_mutex);
                c.x(0, fn(c));
            }
        });
        m.store(table + i * 8, address);
    };
    for (size_t i = 4; i < 236; ++i)
        slot(i, std::to_string(i), [i](auto &) -> std::uint64_t {
            throw Error("Unimplemented JNI table entry " + std::to_string(i));
        });
    slot(4, "GetVersion", [](auto &) { return 0x00010006; });
    slot(6, "FindClass", [&](auto &c) { return class_ref(m.string(c.x(1))); });
    slot(10, "GetSuperclass", [&](auto &c) -> Address {
        auto super = parent(get(c.x(1)).fields.at("name").template get<std::string>());
        return super.empty() ? 0 : class_ref(super);
    });
    slot(11, "IsAssignableFrom", [&](auto &c) {
        return assignable(get(c.x(1)).fields.at("name").template get<std::string>(),
                          get(c.x(2)).fields.at("name").template get<std::string>());
    });
    slot(13, "Throw", [this](auto &c) {
        auto &exception = exceptions_[current_thread()];
        delete_ref(exception);
        exception = global_ref(c.x(1));
        return 0;
    });
    slot(14, "ThrowNew", [this](auto &c) {
        auto &exception = exceptions_[current_thread()];
        delete_ref(exception);
        auto value = object(get(c.x(1)).fields.at("name").template get<std::string>());
        get(value).fields["message"] = runtime_.memory.string(c.x(2));
        exception = global_ref(value);
        delete_ref(value);
        return 0;
    });
    slot(15, "ExceptionOccurred", [this](auto &) { return local_ref(exceptions_[current_thread()]); });
    slot(16, "ExceptionDescribe", [this](auto &) {
        if (exceptions_[current_thread()])
            runtime_.message("Pending Java exception");
        return 0;
    });
    slot(17, "ExceptionClear", [this](auto &) {
        auto &exception = exceptions_[current_thread()];
        delete_ref(exception);
        exception = 0;
        return 0;
    });
    slot(18, "FatalError",
         [&](auto &c) -> std::uint64_t { throw Error("JNI FatalError: " + m.string(c.x(1))); });
    slot(19, "PushLocalFrame", [this](auto &) {
        push_frame();
        return 0;
    });
    slot(20, "PopLocalFrame", [this](auto &c) -> Address { return pop_frame(c.x(1)); });
    for (size_t index : {21, 25, 226})
        slot(index, "NewReference", [this, index](auto &c) -> Address {
            if (!c.x(1))
                return 0;
            auto object = objects_.at(c.x(1));
            auto out = reference(object, index == 25);
            return out;
        });
    for (size_t index : {22, 23, 227})
        slot(index, "DeleteReference", [this](auto &c) {
            delete_ref(c.x(1));
            return 0;
        });
    slot(24, "IsSameObject", [this](auto &c) {
        if (!c.x(1) || !c.x(2))
            return c.x(1) == c.x(2);
        return objects_.at(c.x(1)) == objects_.at(c.x(2));
    });
    slot(26, "EnsureLocalCapacity", [](auto &) { return 0; });
    slot(27, "AllocObject",
         [this](auto &c) { return object(get(c.x(1)).fields.at("name").template get<std::string>()); });
    for (size_t index : {28, 29, 30})
        slot(index, "NewObject", [this, index](auto &c) {
            auto out = object(get(c.x(1)).fields.at("name").template get<std::string>());
            auto receiver = c.x(1);
            c.x(1, out);
            invoke(c, false, static_cast<int>(index - 28), 9);
            c.x(1, receiver);
            return out;
        });
    slot(31, "GetObjectClass", [this](auto &c) { return class_ref(get(c.x(1)).class_name); });
    slot(32, "IsInstanceOf", [this](auto &c) {
        return !c.x(1) || assignable(get(c.x(1)).class_name,
                                     get(c.x(2)).fields.at("name").template get<std::string>());
    });
    for (size_t index : {33, 113, 94, 144})
        slot(index, "GetMemberID", [this, index](auto &c) {
            auto type = get(c.x(1)).fields.at("name").template get<std::string>();
            return method(type, runtime_.memory.string(c.x(2)), runtime_.memory.string(c.x(3)),
                          index == 113 || index == 144);
        });
    for (size_t kind = 0; kind < 10; ++kind)
        for (int variant = 0; variant < 3; ++variant) {
            slot(34 + kind * 3 + variant, "CallMethod",
                 [this, kind, variant](auto &c) { return invoke(c, false, variant, kind); });
            slot(64 + kind * 3 + variant, "CallNonvirtualMethod",
                 [this, kind, variant](auto &c) { return invoke(c, false, variant, kind, true); });
            slot(114 + kind * 3 + variant, "CallStaticMethod",
                 [this, kind, variant](auto &c) { return invoke(c, true, variant, kind); });
        }
    for (size_t kind = 0; kind < 9; ++kind) {
        slot(95 + kind, "GetField", [this, kind](auto &c) { return field_access(c, false, false, kind); });
        slot(104 + kind, "SetField", [this, kind](auto &c) { return field_access(c, false, true, kind); });
        slot(145 + kind, "GetStaticField",
             [this, kind](auto &c) { return field_access(c, true, false, kind); });
        slot(154 + kind, "SetStaticField",
             [this, kind](auto &c) { return field_access(c, true, true, kind); });
    }
    slot(163, "NewString", [&](auto &c) {
        require(c.x(2) < 1 << 24, "JNI string too large");
        auto ref = object("java/lang/String");
        auto &text = get(ref).text;
        text.resize(c.x(2));
        m.read(c.x(1), std::as_writable_bytes(std::span(text)));
        return ref;
    });
    slot(164, "GetStringLength", [this](auto &c) { return get(c.x(1)).text.size(); });
    slot(167, "NewStringUTF", [&](auto &c) {
        if (!c.x(1))
            return Address{};
        auto ref = object("java/lang/String");
        get(ref).text = decode_modified(m.string(c.x(1)));
        return ref;
    });
    slot(168, "GetStringUTFLength", [this](auto &c) { return modified_utf8(get(c.x(1)).text).size(); });
    for (size_t index : {165, 169, 224})
        slot(index, "GetStringChars", [&, index](auto &c) {
            const auto &text = get(c.x(1)).text;
            Address out;
            if (index == 169)
                out = m.string(modified_utf8(text));
            else {
                out = m.allocate(text.size() * 2 + 2);
                m.write(out, std::as_bytes(std::span(text)));
            }
            if (c.x(2))
                m.store(c.x(2), std::uint8_t{1});
            pinned_[out] = {c.x(1), true};
            return out;
        });
    for (size_t index : {166, 170, 225})
        slot(index, "ReleaseStringChars", [&](auto &c) {
            require(pinned_.erase(c.x(2)) != 0, "JNI string release without acquisition");
            m.release(c.x(2));
            return 0;
        });
    slot(171, "GetArrayLength", [this](auto &c) {
        auto &obj = get(c.x(1));
        return obj.fields.contains("length") ? obj.fields.at("length").template get<size_t>()
                                             : obj.elements.size();
    });
    slot(172, "NewObjectArray", [this](auto &c) {
        require(c.x(1) < 1 << 24, "JNI object array too large");
        auto ref = object("[Ljava/lang/Object;");
        get(ref).elements.resize(c.x(1));
        for (auto &e : get(ref).elements)
            if (c.x(3))
                e = reference(objects_.at(c.x(3)), false);
        return ref;
    });
    slot(173, "GetObjectArrayElement", [this](auto &c) -> Address {
        auto &a = get(c.x(1)).elements;
        require(c.x(2) < a.size(), "JNI array index out of bounds");
        return a[c.x(2)] ? reference(objects_.at(a[c.x(2)])) : 0;
    });
    slot(174, "SetObjectArrayElement", [this](auto &c) {
        auto &a = get(c.x(1)).elements;
        require(c.x(2) < a.size(), "JNI array index out of bounds");
        auto value = c.x(3) ? reference(objects_.at(c.x(3)), false) : 0;
        delete_ref(a[c.x(2)]);
        a[c.x(2)] = value;
        return 0;
    });
    constexpr size_t widths[] = {1, 1, 2, 2, 4, 8, 4, 8};
    for (size_t kind = 0; kind < 8; ++kind) {
        auto width = widths[kind];
        slot(175 + kind, "NewPrimitiveArray", [this, width](auto &c) {
            require(c.x(1) < (64 << 20) / width, "JNI primitive array too large");
            auto ref = object("[primitive");
            get(ref).bytes.resize(c.x(1) * width);
            get(ref).fields["length"] = c.x(1);
            return ref;
        });
        slot(183 + kind, "GetArrayElements", [&](auto &c) {
            auto &bytes = get(c.x(1)).bytes;
            auto out = m.allocate(bytes.size());
            m.write(out, bytes);
            if (c.x(2))
                m.store(c.x(2), std::uint8_t{1});
            pinned_[out] = {c.x(1), false};
            return out;
        });
        slot(191 + kind, "ReleaseArrayElements", [&](auto &c) {
            auto it = pinned_.find(c.x(2));
            require(it != pinned_.end() && it->second.first == c.x(1), "JNI array release mismatch");
            if (c.x(3) != 2) {
                auto &b = get(c.x(1)).bytes;
                m.read(c.x(2), b);
            }
            if (c.x(3) != 1) {
                pinned_.erase(it);
                m.release(c.x(2));
            }
            return 0;
        });
        for (int write = 0; write < 2; ++write)
            slot((write ? 207 : 199) + kind, "ArrayRegion", [&, width, write](auto &c) {
                auto &b = get(c.x(1)).bytes;
                auto start = c.x(2), count = c.x(3);
                require(start <= b.size() / width && count <= b.size() / width - start,
                        "JNI array region out of bounds");
                auto region = std::span(b).subspan(start * width, count * width);
                if (write)
                    m.read(c.x(4), region);
                else
                    m.write(c.x(4), region);
                return 0;
            });
    }
    slot(215, "RegisterNatives", [&](auto &c) {
        auto type = get(c.x(1)).fields.at("name").template get<std::string>();
        require(c.x(3) < 65536, "Excessive JNI registration count");
        for (size_t i = 0; i < c.x(3); ++i) {
            auto p = c.x(2) + i * 24;
            register_native(type, m.string(m.load<Address>(p)), m.string(m.load<Address>(p + 8)),
                            m.load<Address>(p + 16));
        }
        return 0;
    });
    slot(216, "UnregisterNatives", [this](auto &c) {
        auto prefix = get(c.x(1)).fields.at("name").template get<std::string>() + "#";
        std::erase_if(natives_, [&](const auto &entry) { return entry.first.starts_with(prefix); });
        return 0;
    });
    slot(217, "MonitorEnter", [this](auto &c) {
        return c.call(runtime_.linker.symbol("pthread_mutex_lock"), std::array{c.x(1)});
    });
    slot(218, "MonitorExit", [this](auto &c) {
        return c.call(runtime_.linker.symbol("pthread_mutex_unlock"), std::array{c.x(1)});
    });
    slot(219, "GetJavaVM", [&](auto &c) {
        m.store(c.x(1), vm_);
        return 0;
    });
    for (size_t index : {220, 221})
        slot(index, "GetStringRegion", [&, index](auto &c) {
            const auto &text = get(c.x(1)).text;
            require(c.x(2) <= text.size() && c.x(3) <= text.size() - c.x(2),
                    "JNI string region out of bounds");
            auto region = text.substr(c.x(2), c.x(3));
            if (index == 220)
                m.write(c.x(4), std::as_bytes(std::span(region)));
            else {
                auto s = modified_utf8(region);
                m.write(c.x(4), std::as_bytes(std::span(s)));
            }
            return 0;
        });
    slot(222, "GetPrimitiveArrayCritical", [&](auto &c) {
        auto &b = get(c.x(1)).bytes;
        auto out = m.allocate(b.size());
        m.write(out, b);
        if (c.x(2))
            m.store(c.x(2), std::uint8_t{1});
        pinned_[out] = {c.x(1), false};
        return out;
    });
    slot(223, "ReleasePrimitiveArrayCritical", [&](auto &c) {
        auto it = pinned_.find(c.x(2));
        require(it != pinned_.end(), "Unmatched JNI critical release");
        if (c.x(3) != 2)
            m.read(c.x(2), get(c.x(1)).bytes);
        if (c.x(3) != 1) {
            pinned_.erase(it);
            m.release(c.x(2));
        }
        return 0;
    });
    slot(228, "ExceptionCheck", [this](auto &) { return exceptions_[current_thread()] != 0; });
    slot(229, "NewDirectByteBuffer", [this](auto &c) {
        runtime_.memory.readable(c.x(1), c.x(2));
        auto ref = object("java/nio/DirectByteBuffer");
        get(ref).fields["address"] = c.x(1);
        get(ref).fields["capacity"] = c.x(2);
        return ref;
    });
    slot(230, "GetDirectBufferAddress",
         [this](auto &c) { return get(c.x(1)).fields.value("address", Address{}); });
    slot(231, "GetDirectBufferCapacity",
         [this](auto &c) { return get(c.x(1)).fields.value("capacity", std::uint64_t(UINT64_MAX)); });
    slot(232, "GetObjectRefType", [](auto &c) { return c.x(1) ? 1 : 0; });
    auto vm_table = m.allocate(8 * 8);
    vm_ = m.allocate(8);
    m.store(vm_, vm_table);
    environments_[runtime.cpu.tls()] = environment_;
    for (size_t index = 3; index < 8; ++index) {
        auto address = runtime.traps.add("JavaVM/" + std::to_string(index), [&, index, table](auto &c) {
            ThreadContext context(this, c.tls());
            std::lock_guard lock(runtime_.state_mutex);
            if (index == 5) {
                while (!frames_[c.tls()].empty())
                    pop_frame();
                frames_.erase(c.tls());
                delete_ref(exceptions_[c.tls()]);
                exceptions_.erase(c.tls());
                auto it = environments_.find(c.tls());
                if (it != environments_.end()) {
                    m.release(it->second);
                    environments_.erase(it);
                }
                c.x(0, 0);
                return;
            }
            if (index == 6) {
                if (c.x(2) != 0x10004 && c.x(2) != 0x10006 && c.x(2) != 0x10008) {
                    m.store(c.x(1), Address{});
                    c.x(0, static_cast<std::uint64_t>(-3ll));
                    return;
                }
                auto it = environments_.find(c.tls());
                m.store(c.x(1), it == environments_.end() ? 0 : it->second);
                c.x(0, it == environments_.end() ? static_cast<std::uint64_t>(-2ll) : 0);
                return;
            }
            if (index == 4 || index == 7) {
                auto &env = environments_[c.tls()];
                if (!env) {
                    env = m.allocate(8);
                    m.store(env, table);
                    push_frame();
                }
                m.store(c.x(1), env);
            }
            c.x(0, 0);
        });
        m.store(vm_table + index * 8, address);
    }

    hook("java/lang/Object", "<init>", "()V", [](auto &) { return 0; });
    hook("java/lang/Class", "getClassLoader", "()Ljava/lang/ClassLoader;",
         [this](auto &) { return object("java/lang/ClassLoader"); });
    hook("java/lang/Class", "getName", "()Ljava/lang/String;", [this](auto &c) {
        auto name = get(c.receiver).fields.at("name").template get<std::string>();
        std::replace(name.begin(), name.end(), '/', '.');
        return string(name);
    });
    hook("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;",
         [this](auto &c) { return class_ref(text(c.arguments[0])); });
    hook("java/lang/ClassLoader", "findClass", "(Ljava/lang/String;)Ljava/lang/Class;",
         [this](auto &c) { return class_ref(text(c.arguments[0])); });
    hook("java/lang/Object", "toString", "()Ljava/lang/String;",
         [this](auto &c) { return string(get(c.receiver).class_name); });
    hook("java/lang/String", "toString", "()Ljava/lang/String;",
         [this](auto &c) { return local_ref(c.receiver); });
    hook("java/lang/String", "length", "()I", [this](auto &c) { return get(c.receiver).text.size(); });
    hook("java/lang/String", "equals", "(Ljava/lang/Object;)Z",
         [this](auto &c) { return c.arguments[0] && get(c.receiver).text == get(c.arguments[0]).text; });
    hook("java/util/ArrayList", "<init>", "()V", [](auto &) { return 0; });
    hook("java/util/ArrayList", "size", "()I", [this](auto &c) { return get(c.receiver).elements.size(); });
    hook("java/util/ArrayList", "get", "(I)Ljava/lang/Object;",
         [this](auto &c) { return local_ref(get(c.receiver).elements.at(c.arguments[0])); });
    hook("java/util/ArrayList", "add", "(Ljava/lang/Object;)Z", [this](auto &c) {
        get(c.receiver)
            .elements.push_back(c.arguments[0] ? reference(objects_.at(c.arguments[0]), false) : 0);
        return 1;
    });
    hook("java/lang/System", "currentTimeMillis", "()J", [](auto &) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    });
    hook("java/lang/System", "nanoTime", "()J", [](auto &) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    });
    field("android/os/Build$VERSION", "SDK_INT", "I", 36);
    field("android/os/Build$VERSION", "RELEASE", "Ljava/lang/String;", string("16"));
    field("android/os/Build", "MODEL", "Ljava/lang/String;", string("Desktop"));
    field("android/os/Build", "MANUFACTURER", "Ljava/lang/String;", string("AndroidPcRuntime"));
}
std::shared_ptr<Object> Jni::make_object() {
    return std::make_shared<Object>();
}
Jni::~Jni() {
    destroying_ = true;
    objects_.clear();
}
} // namespace acp::android
