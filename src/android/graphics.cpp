#include <acp/android.hpp>
namespace acp::android {
namespace {
using U = unsigned;
using I = int;
using F = float;
using B = unsigned char;
using P = void *;
size_t components(U format) {
    switch (format) {
    case 0x1908:
        return 4;
    case 0x1907:
        return 3;
    case 0x190a:
        return 2;
    case 0x1906:
    case 0x1909:
    case 0x1903:
        return 1;
    default:
        throw Error("Unsupported pixel format in GLES bridge");
    }
}
size_t scalar(U type) {
    switch (type) {
    case 0x1400:
    case 0x1401:
        return 1;
    case 0x1402:
    case 0x1403:
    case 0x8d61:
    case 0x140b:
        return 2;
    case 0x1404:
    case 0x1405:
    case 0x1406:
        return 4;
    default:
        throw Error("Unsupported vertex scalar type");
    }
}
} // namespace
struct Graphics::Impl {
    Runtime &runtime;
    std::unique_ptr<host::Library> egl, gles;
    P display{}, surface{}, context{};
    fs::path directory;
    int width = 1280, height = 720, unpack_alignment = 4;
    U array_buffer{};
    std::map<U, Address> strings;
    std::string vendor, renderer, version;
    bool etc1_via_etc2{};
    struct Attribute {
        Address address{};
        I count{}, stride{};
        U type{};
        bool enabled{}, buffer{};
    };
    std::map<U, Attribute> attributes;
    explicit Impl(Runtime &r) : runtime(r) {}
    template <class R, class... A> R call(std::string_view name, A... args) {
        require(gles != nullptr, "GLES called before graphics initialization");
        auto p = gles->symbol(name);
        require(p != nullptr, "ANGLE function unavailable: " + std::string(name));
        return reinterpret_cast<R (*)(A...)>(p)(args...);
    }
    template <class R, class... A> R ecall(std::string_view name, A... args) {
        require(egl != nullptr, "EGL is not initialized");
        auto p = egl->symbol(name);
        require(p != nullptr, "EGL function unavailable: " + std::string(name));
        return reinterpret_cast<R (*)(A...)>(p)(args...);
    }
    size_t image_size(I w, I h, U format, U type) const {
        require(w >= 0 && h >= 0 && w < 32768 && h < 32768, "Invalid GLES image dimensions");
        if (!w || !h)
            return 0;
        auto pixel =
            type == 0x8363 || type == 0x8033 || type == 0x8034 ? 2 : components(format) * scalar(type);
        auto row = size_t(w) * pixel;
        return align_up(row, unpack_alignment) * (h - 1) + row;
    }
    void validate_arrays(I first, I count) {
        require(first >= 0 && count >= 0, "Invalid GLES draw range");
        if (!count)
            return;
        for (const auto &[_, a] : attributes)
            if (a.enabled && !a.buffer) {
                auto element = a.count * scalar(a.type);
                auto stride = a.stride ? static_cast<size_t>(a.stride) : element;
                runtime.memory.readable(a.address, (size_t(first) + count - 1) * stride + element);
            }
    }
};
Graphics::Graphics(Runtime &r) : impl_(std::make_unique<Impl>(r)) {
    auto *p = impl_.get();
    auto &m = r.memory;
    auto bind = [&](const char *name, auto fn) {
        r.bind_void(name, [fn = std::move(fn)](guest::Cpu &c) { fn(c); });
    };
    bind("glGetString", [p, &m](auto &c) {
        auto name = static_cast<U>(c.x(0));
        auto it = p->strings.find(name);
        if (it == p->strings.end()) {
            auto text = p->call<const unsigned char *>("glGetString", name);
            std::string value = text ? reinterpret_cast<const char *>(text) : "";
            if (name == 0x1f03 && p->etc1_via_etc2)
                value += " GL_OES_compressed_ETC1_RGB8_texture";
            c.x(0, text ? m.string(value) : 0);
            if (text)
                p->strings[name] = c.x(0);
        } else
            c.x(0, it->second);
    });
    bind("glGetIntegerv", [p, &m](auto &c) {
        auto name = static_cast<U>(c.x(0));
        auto n = name == 0x0ba2 || name == 0x0c10 ? 4 : name == 0x0d3a ? 2 : 1;
        if (name == 0x86a2 && p->etc1_via_etc2) {
            I count{};
            p->call<void>("glGetIntegerv", name, &count);
            m.store(c.x(1), count + 1);
            return;
        }
        if (name == 0x86a3) {
            I count{};
            p->call<void>("glGetIntegerv", U{0x86a2}, &count);
            require(count >= 0 && count < 4096, "Invalid compressed texture format count");
            n = count + (p->etc1_via_etc2 ? 1 : 0);
            auto out = m.writable(c.x(1), size_t(n) * 4);
            p->call<void>("glGetIntegerv", name, reinterpret_cast<I *>(out.data()));
            if (p->etc1_via_etc2)
                m.store(c.x(1) + size_t(count) * 4, U{0x8d64});
            return;
        }
        if (name == 0x8df8 || name == 0x87ff) {
            I count{};
            p->call<void>("glGetIntegerv", name == 0x8df8 ? U{0x8df9} : U{0x87fe}, &count);
            require(count >= 0 && count < 4096, "Invalid GLES binary format count");
            n = count;
        }
        auto out = m.writable(c.x(1), n * 4);
        p->call<void>("glGetIntegerv", name, reinterpret_cast<I *>(out.data()));
    });
    bind("glGetError", [p](auto &c) { c.x(0, p->call<U>("glGetError")); });
    for (const auto *name :
         {"glEnable", "glDisable", "glActiveTexture", "glFrontFace", "glCullFace", "glDepthFunc",
          "glUseProgram", "glDeleteProgram", "glDeleteShader", "glGenerateMipmap"})
        bind(name, [p, name](auto &c) { p->call<void>(name, static_cast<U>(c.x(0))); });
    for (const auto *name : {"glIsTexture", "glIsProgram", "glIsShader"})
        bind(name, [p, name](auto &c) { c.x(0, p->call<B>(name, static_cast<U>(c.x(0)))); });
    for (const auto *name : {"glBindTexture", "glBindFramebuffer", "glBindRenderbuffer", "glBlendFunc",
                             "glAttachShader", "glDetachShader", "glBindBuffer"})
        bind(name, [p, name](auto &c) {
            if (std::string_view(name) == "glBindBuffer" && c.x(0) == 0x8892)
                p->array_buffer = static_cast<U>(c.x(1));
            p->call<void>(name, static_cast<U>(c.x(0)), static_cast<U>(c.x(1)));
        });
    for (const auto *name :
         {"glGenTextures", "glGenFramebuffers", "glGenRenderbuffers", "glGenBuffers", "glDeleteTextures",
          "glDeleteFramebuffers", "glDeleteRenderbuffers", "glDeleteBuffers"})
        bind(name, [p, &m, name](auto &c) {
            require(c.x(0) < 1 << 20, "Excessive GLES object count");
            auto out = m.writable(c.x(1), c.x(0) * 4);
            p->call<void>(name, static_cast<I>(c.x(0)), reinterpret_cast<U *>(out.data()));
        });
    bind("glPixelStorei", [p](auto &c) {
        if (c.x(0) == 0x0cf5) {
            require(c.x(1) == 1 || c.x(1) == 2 || c.x(1) == 4 || c.x(1) == 8, "Invalid pixel alignment");
            p->unpack_alignment = static_cast<int>(c.x(1));
        }
        p->call<void>("glPixelStorei", static_cast<U>(c.x(0)), static_cast<I>(c.x(1)));
    });
    bind("glTexParameteri", [p](auto &c) {
        p->call<void>("glTexParameteri", static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                      static_cast<I>(c.x(2)));
    });
    bind("glTexParameterf", [p](auto &c) {
        p->call<void>("glTexParameterf", static_cast<U>(c.x(0)), static_cast<U>(c.x(1)), c.f32(0));
    });
    bind("glTexImage2D", [p, &m](auto &c) {
        auto bytes = p->image_size(static_cast<I>(c.x(3)), static_cast<I>(c.x(4)), static_cast<U>(c.x(6)),
                                   static_cast<U>(c.x(7)));
        auto a = c.argument(8);
        const void *data = a ? m.readable(a, bytes).data() : nullptr;
        p->call<void>("glTexImage2D", static_cast<U>(c.x(0)), static_cast<I>(c.x(1)),
                      static_cast<I>(c.x(2)), static_cast<I>(c.x(3)), static_cast<I>(c.x(4)),
                      static_cast<I>(c.x(5)), static_cast<U>(c.x(6)), static_cast<U>(c.x(7)), data);
    });
    bind("glTexSubImage2D", [p, &m](auto &c) {
        auto bytes = p->image_size(static_cast<I>(c.x(4)), static_cast<I>(c.x(5)), static_cast<U>(c.x(6)),
                                   static_cast<U>(c.x(7)));
        auto data = m.readable(c.argument(8), bytes).data();
        p->call<void>("glTexSubImage2D", static_cast<U>(c.x(0)), static_cast<I>(c.x(1)),
                      static_cast<I>(c.x(2)), static_cast<I>(c.x(3)), static_cast<I>(c.x(4)),
                      static_cast<I>(c.x(5)), static_cast<U>(c.x(6)), static_cast<U>(c.x(7)),
                      static_cast<const void *>(data));
    });
    bind("glCompressedTexImage2D", [p, &m](auto &c) {
        auto data = c.x(7) ? m.readable(c.x(7), c.x(6)).data() : nullptr;
        p->call<void>("glCompressedTexImage2D", static_cast<U>(c.x(0)), static_cast<I>(c.x(1)),
                      static_cast<U>(c.x(2) == 0x8d64 && p->etc1_via_etc2 ? 0x9274 : c.x(2)),
                      static_cast<I>(c.x(3)), static_cast<I>(c.x(4)), static_cast<I>(c.x(5)),
                      static_cast<I>(c.x(6)), static_cast<const void *>(data));
    });
    bind("glCreateProgram", [p](auto &c) { c.x(0, p->call<U>("glCreateProgram")); });
    bind("glCreateShader", [p](auto &c) { c.x(0, p->call<U>("glCreateShader", static_cast<U>(c.x(0)))); });
    bind("glShaderSource", [p, &m](auto &c) {
        require(c.x(1) < 65536, "Excessive shader source count");
        std::vector<std::string> text;
        std::vector<const char *> ptrs;
        std::vector<I> sizes;
        text.reserve(c.x(1));
        for (size_t i = 0; i < c.x(1); ++i) {
            auto address = m.load<Address>(c.x(2) + i * 8);
            auto n = c.x(3) ? m.load<I>(c.x(3) + i * 4) : -1;
            if (n < 0)
                text.push_back(m.string(address, 16 << 20));
            else {
                auto b = m.readable(address, n);
                text.emplace_back(reinterpret_cast<const char *>(b.data()), b.size());
            }
        }
        for (auto &s : text) {
            ptrs.push_back(s.c_str());
            sizes.push_back(static_cast<I>(s.size()));
        }
        p->call<void>("glShaderSource", static_cast<U>(c.x(0)), static_cast<I>(ptrs.size()),
                      static_cast<const char *const *>(ptrs.data()), static_cast<const I *>(sizes.data()));
    });
    for (const auto *name : {"glCompileShader", "glLinkProgram", "glValidateProgram"})
        bind(name, [p, name](auto &c) { p->call<void>(name, static_cast<U>(c.x(0))); });
    bind("glBindAttribLocation", [p, &m](auto &c) {
        p->call<void>("glBindAttribLocation", static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                      m.string(c.x(2)).c_str());
    });
    for (const auto *name : {"glGetUniformLocation", "glGetAttribLocation"})
        bind(name, [p, &m, name](auto &c) {
            auto result = p->call<I>(name, static_cast<U>(c.x(0)), m.string(c.x(1)).c_str());
            c.x(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
        });
    for (const auto *name : {"glGetShaderiv", "glGetProgramiv"})
        bind(name, [p, &m, name](auto &c) {
            auto out = m.writable(c.x(2), 4);
            p->call<void>(name, static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                          reinterpret_cast<I *>(out.data()));
        });
    for (const auto *name : {"glGetShaderInfoLog", "glGetProgramInfoLog"})
        bind(name, [p, &m, name](auto &c) {
            require(c.x(1) < 16 << 20, "Excessive GLES log size");
            I length{};
            auto out = m.writable(c.x(3), c.x(1));
            p->call<void>(name, static_cast<U>(c.x(0)), static_cast<I>(out.size()), &length,
                          reinterpret_cast<char *>(out.data()));
            if (c.x(2))
                m.store(c.x(2), length);
        });
    bind("glUniform1i",
         [p](auto &c) { p->call<void>("glUniform1i", static_cast<I>(c.x(0)), static_cast<I>(c.x(1))); });
    bind("glUniform1f", [p](auto &c) { p->call<void>("glUniform1f", static_cast<I>(c.x(0)), c.f32(0)); });
    for (const auto *name : {"glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv"})
        bind(name, [p, &m, name](auto &c) {
            auto width = static_cast<size_t>(name[9] - '0');
            auto b = m.readable(c.x(2), c.x(1) * width * 4);
            p->call<void>(name, static_cast<I>(c.x(0)), static_cast<I>(c.x(1)),
                          reinterpret_cast<const F *>(b.data()));
        });
    bind("glUniformMatrix4fv", [p, &m](auto &c) {
        auto b = m.readable(c.x(3), c.x(1) * 64);
        p->call<void>("glUniformMatrix4fv", static_cast<I>(c.x(0)), static_cast<I>(c.x(1)),
                      static_cast<B>(c.x(2)), reinterpret_cast<const F *>(b.data()));
    });
    bind("glVertexAttribPointer", [p, &m](auto &c) {
        auto id = static_cast<U>(c.x(0));
        auto &a = p->attributes[id];
        a.address = c.x(5);
        a.count = static_cast<I>(c.x(1));
        a.type = static_cast<U>(c.x(2));
        a.stride = static_cast<I>(c.x(4));
        a.buffer = p->array_buffer != 0;
        require(a.count >= 1 && a.count <= 4 && a.stride >= 0, "Invalid GLES vertex attribute");
        auto pointer = a.buffer    ? reinterpret_cast<const void *>(static_cast<uintptr_t>(a.address))
                       : a.address ? m.readable(a.address, 1).data()
                                   : nullptr;
        p->call<void>("glVertexAttribPointer", id, a.count, a.type, static_cast<B>(c.x(3)), a.stride,
                      pointer);
    });
    for (const auto *name : {"glEnableVertexAttribArray", "glDisableVertexAttribArray"})
        bind(name, [p, name](auto &c) {
            auto index = static_cast<U>(c.x(0));
            p->attributes[index].enabled = std::string_view(name) == "glEnableVertexAttribArray";
            p->call<void>(name, index);
        });
    bind("glDrawArrays", [p](auto &c) {
        p->validate_arrays(static_cast<I>(c.x(1)), static_cast<I>(c.x(2)));
        p->call<void>("glDrawArrays", static_cast<U>(c.x(0)), static_cast<I>(c.x(1)),
                      static_cast<I>(c.x(2)));
    });
    bind("glClearColor",
         [p](auto &c) { p->call<void>("glClearColor", c.f32(0), c.f32(1), c.f32(2), c.f32(3)); });
    bind("glClear", [p](auto &c) { p->call<void>("glClear", static_cast<U>(c.x(0))); });
    for (const auto *name : {"glClearDepthf", "glLineWidth"})
        bind(name, [p, name](auto &c) { p->call<void>(name, c.f32(0)); });
    bind("glDepthRangef", [p](auto &c) { p->call<void>("glDepthRangef", c.f32(0), c.f32(1)); });
    bind("glDepthMask", [p](auto &c) { p->call<void>("glDepthMask", static_cast<B>(c.x(0))); });
    bind("glColorMask", [p](auto &c) {
        p->call<void>("glColorMask", static_cast<B>(c.x(0)), static_cast<B>(c.x(1)), static_cast<B>(c.x(2)),
                      static_cast<B>(c.x(3)));
    });
    for (const auto *name : {"glViewport", "glScissor"})
        bind(name, [p, name](auto &c) {
            p->call<void>(name, static_cast<I>(c.x(0)), static_cast<I>(c.x(1)), static_cast<I>(c.x(2)),
                          static_cast<I>(c.x(3)));
        });
    bind("glFramebufferTexture2D", [p](auto &c) {
        p->call<void>("glFramebufferTexture2D", static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                      static_cast<U>(c.x(2)), static_cast<U>(c.x(3)), static_cast<I>(c.x(4)));
    });
    bind("glCheckFramebufferStatus",
         [p](auto &c) { c.x(0, p->call<U>("glCheckFramebufferStatus", static_cast<U>(c.x(0)))); });
    bind("glFlush", [p](auto &) { p->call<void>("glFlush"); });
    bind("glFinish", [p](auto &) { p->call<void>("glFinish"); });
    bind("eglGetProcAddress", [&r, &m](auto &c) { c.x(0, r.linker.symbol(m.string(c.x(0)))); });
    bind("eglGetCurrentContext", [p](auto &c) { c.x(0, p->context ? 1 : 0); });
    bind("eglGetCurrentDisplay", [p](auto &c) { c.x(0, p->display ? 1 : 0); });
}
void Graphics::open(void *window, const std::optional<fs::path> &path) {
    auto &p = *impl_;
    auto directory = path ? path : host::development_angle_directory();
    require(directory.has_value(),
            "ANGLE DLLs not found; supply --angle-dir or run tools/package-angle.ps1");
    p.directory = *directory;
    p.gles = std::make_unique<host::Library>(*directory / "libGLESv2.dll");
    p.egl = std::make_unique<host::Library>(*directory / "libEGL.dll");
    auto platform = p.ecall<void *>("eglGetProcAddress", "eglGetPlatformDisplayEXT");
    const I attributes[] = {0x3203, 0x3208, 0x3038};
    p.display = platform ? reinterpret_cast<P (*)(U, P, const I *)>(platform)(0x3202, nullptr, attributes)
                         : p.ecall<P>("eglGetDisplay", static_cast<P>(nullptr));
    I major{}, minor{};
    require(p.display && p.ecall<U>("eglInitialize", p.display, &major, &minor) != 0,
            "ANGLE EGL initialization failed");
    require(p.ecall<U>("eglBindAPI", U{0x30a0}) != 0, "Cannot bind GLES API");
    const I config_attributes[] = {0x3024, 8,      0x3023, 8,      0x3022, 8,      0x3021, 8,     0x3025,
                                   24,     0x3026, 8,      0x3033, 4,      0x3040, 4,      0x3038};
    P config{};
    I count{};
    require(p.ecall<U>("eglChooseConfig", p.display, config_attributes, &config, 1, &count) != 0 &&
                count > 0,
            "No suitable ANGLE GLES2 configuration");
    p.surface =
        p.ecall<P>("eglCreateWindowSurface", p.display, config, window, static_cast<const I *>(nullptr));
    const I context_attributes[] = {0x3098, 2, 0x3038};
    p.context =
        p.ecall<P>("eglCreateContext", p.display, config, static_cast<P>(nullptr), context_attributes);
    require(p.surface && p.context, "ANGLE surface/context creation failed");
    require(p.ecall<U>("eglMakeCurrent", p.display, p.surface, p.surface, p.context) != 0,
            "Cannot activate ANGLE context");
    p.ecall<U>("eglSwapInterval", p.display, 1);
    auto version = reinterpret_cast<const char *>(p.call<const unsigned char *>("glGetString", U{0x1f02}));
    auto vendor = reinterpret_cast<const char *>(p.call<const unsigned char *>("glGetString", U{0x1f00}));
    auto renderer = reinterpret_cast<const char *>(p.call<const unsigned char *>("glGetString", U{0x1f01}));
    p.version = version ? version : "";
    p.vendor = vendor ? vendor : "";
    p.renderer = renderer ? renderer : "";
    auto extensions =
        reinterpret_cast<const char *>(p.call<const unsigned char *>("glGetString", U{0x1f03}));
    // ETC2 RGB decodes legacy ETC1 blocks unchanged (Khronos Data Format, ETC2 chapter).
    p.etc1_via_etc2 =
        version && extensions && std::string_view(version).starts_with("OpenGL ES 3.") &&
        std::string_view(extensions).find("GL_OES_compressed_ETC1_RGB8_texture") == std::string_view::npos;
    p.runtime.message("ANGLE initialized from " + utf8(*directory));
}
Graphics::~Graphics() {
    auto &p = *impl_;
    if (p.display && p.egl) {
        try {
            p.ecall<U>("eglMakeCurrent", p.display, static_cast<P>(nullptr), static_cast<P>(nullptr),
                       static_cast<P>(nullptr));
            if (p.context)
                p.ecall<U>("eglDestroyContext", p.display, p.context);
            if (p.surface)
                p.ecall<U>("eglDestroySurface", p.display, p.surface);
            p.ecall<U>("eglTerminate", p.display);
        } catch (...) {
        }
    }
}
void Graphics::present() {
    auto &p = *impl_;
    require(p.ecall<U>("eglSwapBuffers", p.display, p.surface) != 0, "EGL presentation failed");
}
void Graphics::resize(int w, int h) {
    impl_->width = w;
    impl_->height = h;
}
void Graphics::finish_frame() {}
Bytes Graphics::read_rgba(int width, int height) {
    require(width > 0 && height > 0 && std::uint64_t(width) * height <= 16'777'216,
            "GLES capture dimensions are invalid");
    Bytes pixels(static_cast<size_t>(width) * height * 4);
    impl_->call<void>("glReadPixels", 0, 0, width, height, U{0x1908}, U{0x1401},
                      static_cast<void *>(pixels.data()));
    require(impl_->call<U>("glGetError") == 0, "GLES capture failed");
    return pixels;
}
Json Graphics::describe() const {
    const auto &p = *impl_;
    return {{"initialized", p.context != nullptr},
            {"backend", "ANGLE/D3D11"},
            {"etc1_via_etc2", p.etc1_via_etc2},
            {"vendor", p.vendor},
            {"renderer", p.renderer},
            {"version", p.version},
            {"width", p.width},
            {"height", p.height},
            {"directory", utf8(p.directory)}};
}
} // namespace acp::android
