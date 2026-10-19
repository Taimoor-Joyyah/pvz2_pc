#include <acp/android.hpp>

#include <set>
namespace acp::android {
namespace {
using U = unsigned;
using I = int;
using F = float;
using B = unsigned char;
using P = void *;
template <class Signature> struct EntryPoint;
template <class R, class... A> struct EntryPoint<R(A...)> {
    R (*target)(A...){};
    R operator()(A... args) const {
        require(target != nullptr, "Graphics called before entry-point initialization");
        return target(args...);
    }
    void load(const host::Library &library, const char *name) {
        auto address = library.symbol(name);
        if (!address)
            throw Error("ANGLE function unavailable: " + std::string(name));
        target = reinterpret_cast<R (*)(A...)>(address);
    }
};
struct Functions {
#define GLES_FUNCTION(name, signature) EntryPoint<signature> name;
#define EGL_FUNCTION(name, signature) GLES_FUNCTION(name, signature)
#include "graphics_functions.inc"
#undef EGL_FUNCTION
#undef GLES_FUNCTION
    void load(const host::Library &gles, const host::Library &egl) {
#define GLES_FUNCTION(name, signature) name.load(gles, #name);
#define EGL_FUNCTION(name, signature) name.load(egl, #name);
#include "graphics_functions.inc"
#undef EGL_FUNCTION
#undef GLES_FUNCTION
    }
};
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
    Functions functions;
    P display{}, surface{}, context{};
    fs::path directory;
    int width = 1280, height = 720, unpack_alignment = 4;
    U array_buffer{};
    U guest_framebuffer{}, guest_texture{}, guest_depth{};
    int guest_width{}, guest_height{};
    std::set<std::array<int, 4>> viewports;
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
            auto text = p->functions.glGetString(name);
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
            p->functions.glGetIntegerv(name, &count);
            m.store(c.x(1), count + 1);
            return;
        }
        if (name == 0x86a3) {
            I count{};
            p->functions.glGetIntegerv(U{0x86a2}, &count);
            require(count >= 0 && count < 4096, "Invalid compressed texture format count");
            n = count + (p->etc1_via_etc2 ? 1 : 0);
            auto out = m.writable(c.x(1), size_t(n) * 4);
            p->functions.glGetIntegerv(name, reinterpret_cast<I *>(out.data()));
            if (p->etc1_via_etc2)
                m.store(c.x(1) + size_t(count) * 4, U{0x8d64});
            return;
        }
        if (name == 0x8df8 || name == 0x87ff) {
            I count{};
            p->functions.glGetIntegerv(name == 0x8df8 ? U{0x8df9} : U{0x87fe}, &count);
            require(count >= 0 && count < 4096, "Invalid GLES binary format count");
            n = count;
        }
        auto out = m.writable(c.x(1), n * 4);
        p->functions.glGetIntegerv(name, reinterpret_cast<I *>(out.data()));
        if (p->guest_framebuffer && (name == 0x8ca6 || name == 0x8caa) &&
            m.load<U>(c.x(1)) == p->guest_framebuffer)
            m.store(c.x(1), U{0});
    });
    bind("glGetError", [p](auto &c) { c.x(0, p->functions.glGetError()); });
#define GL_ENTRY(name)                                                                                     \
    std::pair {                                                                                            \
        #name, &Functions::name                                                                            \
    }
    for (auto [name, function] :
         {GL_ENTRY(glEnable), GL_ENTRY(glDisable), GL_ENTRY(glActiveTexture), GL_ENTRY(glFrontFace),
          GL_ENTRY(glCullFace), GL_ENTRY(glDepthFunc), GL_ENTRY(glUseProgram), GL_ENTRY(glDeleteProgram),
          GL_ENTRY(glDeleteShader), GL_ENTRY(glGenerateMipmap)})
        bind(name, [p, function](auto &c) { (p->functions.*function)(static_cast<U>(c.x(0))); });
    for (auto [name, function] : {GL_ENTRY(glIsTexture), GL_ENTRY(glIsProgram), GL_ENTRY(glIsShader)})
        bind(name, [p, function](auto &c) { c.x(0, (p->functions.*function)(static_cast<U>(c.x(0)))); });
    for (auto [name, function] :
         {GL_ENTRY(glBindTexture), GL_ENTRY(glBindFramebuffer), GL_ENTRY(glBindRenderbuffer),
          GL_ENTRY(glBlendFunc), GL_ENTRY(glAttachShader), GL_ENTRY(glDetachShader),
          GL_ENTRY(glBindBuffer)})
        bind(name, [p, function, buffer = function == &Functions::glBindBuffer,
                    framebuffer = function == &Functions::glBindFramebuffer](auto &c) {
            if (buffer && c.x(0) == 0x8892)
                p->array_buffer = static_cast<U>(c.x(1));
            auto object = static_cast<U>(c.x(1));
            if (framebuffer && object == 0)
                object = p->guest_framebuffer;
            (p->functions.*function)(static_cast<U>(c.x(0)), object);
        });
    for (auto [name, function] : {GL_ENTRY(glGenTextures), GL_ENTRY(glGenFramebuffers),
                                  GL_ENTRY(glGenRenderbuffers), GL_ENTRY(glGenBuffers)})
        bind(name, [p, &m, function](auto &c) {
            require(c.x(0) < 1 << 20, "Excessive GLES object count");
            auto out = m.writable(c.x(1), c.x(0) * 4);
            (p->functions.*function)(static_cast<I>(c.x(0)), reinterpret_cast<U *>(out.data()));
        });
    for (auto [name, function] : {GL_ENTRY(glDeleteTextures), GL_ENTRY(glDeleteFramebuffers),
                                  GL_ENTRY(glDeleteRenderbuffers), GL_ENTRY(glDeleteBuffers)})
        bind(name, [p, &m, function](auto &c) {
            require(c.x(0) < 1 << 20, "Excessive GLES object count");
            auto objects = m.readable(c.x(1), c.x(0) * 4);
            (p->functions.*function)(static_cast<I>(c.x(0)), reinterpret_cast<const U *>(objects.data()));
        });
    bind("glPixelStorei", [p](auto &c) {
        if (c.x(0) == 0x0cf5) {
            require(c.x(1) == 1 || c.x(1) == 2 || c.x(1) == 4 || c.x(1) == 8, "Invalid pixel alignment");
            p->unpack_alignment = static_cast<int>(c.x(1));
        }
        p->functions.glPixelStorei(static_cast<U>(c.x(0)), static_cast<I>(c.x(1)));
    });
    bind("glTexParameteri", [p](auto &c) {
        p->functions.glTexParameteri(static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                                     static_cast<I>(c.x(2)));
    });
    bind("glTexParameterf", [p](auto &c) {
        p->functions.glTexParameterf(static_cast<U>(c.x(0)), static_cast<U>(c.x(1)), c.f32(0));
    });
    bind("glTexImage2D", [p, &m](auto &c) {
        auto bytes = p->image_size(static_cast<I>(c.x(3)), static_cast<I>(c.x(4)), static_cast<U>(c.x(6)),
                                   static_cast<U>(c.x(7)));
        auto a = c.argument(8);
        const void *data = a ? m.readable(a, bytes).data() : nullptr;
        p->functions.glTexImage2D(static_cast<U>(c.x(0)), static_cast<I>(c.x(1)), static_cast<I>(c.x(2)),
                                  static_cast<I>(c.x(3)), static_cast<I>(c.x(4)), static_cast<I>(c.x(5)),
                                  static_cast<U>(c.x(6)), static_cast<U>(c.x(7)), data);
    });
    bind("glTexSubImage2D", [p, &m](auto &c) {
        auto bytes = p->image_size(static_cast<I>(c.x(4)), static_cast<I>(c.x(5)), static_cast<U>(c.x(6)),
                                   static_cast<U>(c.x(7)));
        auto data = m.readable(c.argument(8), bytes).data();
        p->functions.glTexSubImage2D(static_cast<U>(c.x(0)), static_cast<I>(c.x(1)), static_cast<I>(c.x(2)),
                                     static_cast<I>(c.x(3)), static_cast<I>(c.x(4)), static_cast<I>(c.x(5)),
                                     static_cast<U>(c.x(6)), static_cast<U>(c.x(7)),
                                     static_cast<const void *>(data));
    });
    bind("glCompressedTexImage2D", [p, &m](auto &c) {
        auto data = c.x(7) ? m.readable(c.x(7), c.x(6)).data() : nullptr;
        p->functions.glCompressedTexImage2D(
            static_cast<U>(c.x(0)), static_cast<I>(c.x(1)),
            static_cast<U>(c.x(2) == 0x8d64 && p->etc1_via_etc2 ? 0x9274 : c.x(2)), static_cast<I>(c.x(3)),
            static_cast<I>(c.x(4)), static_cast<I>(c.x(5)), static_cast<I>(c.x(6)),
            static_cast<const void *>(data));
    });
    bind("glCreateProgram", [p](auto &c) { c.x(0, p->functions.glCreateProgram()); });
    bind("glCreateShader", [p](auto &c) { c.x(0, p->functions.glCreateShader(static_cast<U>(c.x(0)))); });
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
        p->functions.glShaderSource(static_cast<U>(c.x(0)), static_cast<I>(ptrs.size()),
                                    static_cast<const char *const *>(ptrs.data()),
                                    static_cast<const I *>(sizes.data()));
    });
    for (auto [name, function] :
         {GL_ENTRY(glCompileShader), GL_ENTRY(glLinkProgram), GL_ENTRY(glValidateProgram)})
        bind(name, [p, function](auto &c) { (p->functions.*function)(static_cast<U>(c.x(0))); });
    bind("glBindAttribLocation", [p, &m](auto &c) {
        p->functions.glBindAttribLocation(static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                                          m.string(c.x(2)).c_str());
    });
    for (auto [name, function] : {GL_ENTRY(glGetUniformLocation), GL_ENTRY(glGetAttribLocation)})
        bind(name, [p, &m, function](auto &c) {
            auto result = (p->functions.*function)(static_cast<U>(c.x(0)), m.string(c.x(1)).c_str());
            c.x(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(result)));
        });
    for (auto [name, function] : {GL_ENTRY(glGetShaderiv), GL_ENTRY(glGetProgramiv)})
        bind(name, [p, &m, function](auto &c) {
            auto out = m.writable(c.x(2), 4);
            (p->functions.*function)(static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                                     reinterpret_cast<I *>(out.data()));
        });
    for (auto [name, function] : {GL_ENTRY(glGetShaderInfoLog), GL_ENTRY(glGetProgramInfoLog)})
        bind(name, [p, &m, function](auto &c) {
            require(c.x(1) < 16 << 20, "Excessive GLES log size");
            I length{};
            auto out = m.writable(c.x(3), c.x(1));
            (p->functions.*function)(static_cast<U>(c.x(0)), static_cast<I>(out.size()), &length,
                                     reinterpret_cast<char *>(out.data()));
            if (c.x(2))
                m.store(c.x(2), length);
        });
    bind("glUniform1i",
         [p](auto &c) { p->functions.glUniform1i(static_cast<I>(c.x(0)), static_cast<I>(c.x(1))); });
    bind("glUniform1f", [p](auto &c) { p->functions.glUniform1f(static_cast<I>(c.x(0)), c.f32(0)); });
    for (auto [name, function] :
         {GL_ENTRY(glUniform1fv), GL_ENTRY(glUniform2fv), GL_ENTRY(glUniform3fv), GL_ENTRY(glUniform4fv)})
        bind(name, [p, &m, function, width = static_cast<size_t>(name[9] - '0')](auto &c) {
            auto b = m.readable(c.x(2), c.x(1) * width * 4);
            (p->functions.*function)(static_cast<I>(c.x(0)), static_cast<I>(c.x(1)),
                                     reinterpret_cast<const F *>(b.data()));
        });
    bind("glUniformMatrix4fv", [p, &m](auto &c) {
        auto b = m.readable(c.x(3), c.x(1) * 64);
        p->functions.glUniformMatrix4fv(static_cast<I>(c.x(0)), static_cast<I>(c.x(1)),
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
        p->functions.glVertexAttribPointer(id, a.count, a.type, static_cast<B>(c.x(3)), a.stride, pointer);
    });
    for (auto [name, function] :
         {GL_ENTRY(glEnableVertexAttribArray), GL_ENTRY(glDisableVertexAttribArray)})
        bind(name, [p, function, enabled = function == &Functions::glEnableVertexAttribArray](auto &c) {
            auto index = static_cast<U>(c.x(0));
            p->attributes[index].enabled = enabled;
            (p->functions.*function)(index);
        });
    bind("glDrawArrays", [p](auto &c) {
        p->validate_arrays(static_cast<I>(c.x(1)), static_cast<I>(c.x(2)));
        p->functions.glDrawArrays(static_cast<U>(c.x(0)), static_cast<I>(c.x(1)), static_cast<I>(c.x(2)));
    });
    bind("glClearColor",
         [p](auto &c) { p->functions.glClearColor(c.f32(0), c.f32(1), c.f32(2), c.f32(3)); });
    bind("glClear", [p](auto &c) { p->functions.glClear(static_cast<U>(c.x(0))); });
    for (auto [name, function] : {GL_ENTRY(glClearDepthf), GL_ENTRY(glLineWidth)})
        bind(name, [p, function](auto &c) { (p->functions.*function)(c.f32(0)); });
    bind("glDepthRangef", [p](auto &c) { p->functions.glDepthRangef(c.f32(0), c.f32(1)); });
    bind("glDepthMask", [p](auto &c) { p->functions.glDepthMask(static_cast<B>(c.x(0))); });
    bind("glColorMask", [p](auto &c) {
        p->functions.glColorMask(static_cast<B>(c.x(0)), static_cast<B>(c.x(1)), static_cast<B>(c.x(2)),
                                 static_cast<B>(c.x(3)));
    });
    for (auto [name, function] : {GL_ENTRY(glViewport), GL_ENTRY(glScissor)})
        bind(name, [p, function, viewport = function == &Functions::glViewport](auto &c) {
            if (viewport) {
                std::array<int, 4> value{static_cast<I>(c.x(0)), static_cast<I>(c.x(1)),
                                         static_cast<I>(c.x(2)), static_cast<I>(c.x(3))};
                if (p->viewports.size() < 64 && p->viewports.insert(value).second) {
                    I framebuffer{};
                    p->functions.glGetIntegerv(U{0x8ca6}, &framebuffer);
                    p->runtime.message(std::format("[GLES] Guest viewport: {},{} {}x{}, framebuffer {}",
                                                   value[0], value[1], value[2], value[3],
                                                   framebuffer == p->guest_framebuffer ? 0 : framebuffer));
                }
            }
            (p->functions.*function)(static_cast<I>(c.x(0)), static_cast<I>(c.x(1)), static_cast<I>(c.x(2)),
                                     static_cast<I>(c.x(3)));
        });
    bind("glFramebufferTexture2D", [p](auto &c) {
        p->functions.glFramebufferTexture2D(static_cast<U>(c.x(0)), static_cast<U>(c.x(1)),
                                            static_cast<U>(c.x(2)), static_cast<U>(c.x(3)),
                                            static_cast<I>(c.x(4)));
    });
    bind("glCheckFramebufferStatus",
         [p](auto &c) { c.x(0, p->functions.glCheckFramebufferStatus(static_cast<U>(c.x(0)))); });
    bind("glFlush", [p](auto &) { p->functions.glFlush(); });
    bind("glFinish", [p](auto &) { p->functions.glFinish(); });
    bind("eglGetProcAddress", [&r, &m](auto &c) { c.x(0, r.linker.symbol(m.string(c.x(0)))); });
    bind("eglGetCurrentContext", [p](auto &c) { c.x(0, p->context ? 1 : 0); });
    bind("eglGetCurrentDisplay", [p](auto &c) { c.x(0, p->display ? 1 : 0); });
#undef GL_ENTRY
}
void Graphics::open(void *window, const std::optional<fs::path> &path) {
    auto &p = *impl_;
    require(!p.gles && !p.egl, "Graphics is already initialized");
    auto directory = path ? path : host::development_angle_directory();
    require(directory.has_value(),
            "ANGLE DLLs not found; supply --angle-dir or run tools/package-angle.ps1");
    p.directory = *directory;
    p.gles = std::make_unique<host::Library>(*directory / "libGLESv2.dll");
    p.egl = std::make_unique<host::Library>(*directory / "libEGL.dll");
    p.functions.load(*p.gles, *p.egl);
    auto platform = p.functions.eglGetProcAddress("eglGetPlatformDisplayEXT");
    const I attributes[] = {0x3203, 0x3208, 0x3038};
    p.display = platform ? reinterpret_cast<P (*)(U, P, const I *)>(platform)(0x3202, nullptr, attributes)
                         : p.functions.eglGetDisplay(static_cast<P>(nullptr));
    I major{}, minor{};
    require(p.display && p.functions.eglInitialize(p.display, &major, &minor) != 0,
            "ANGLE EGL initialization failed");
    require(p.functions.eglBindAPI(U{0x30a0}) != 0, "Cannot bind GLES API");
    const I config_attributes[] = {0x3024, 8,      0x3023, 8,      0x3022, 8,      0x3021, 8,     0x3025,
                                   24,     0x3026, 8,      0x3033, 4,      0x3040, 4,      0x3038};
    P config{};
    I count{};
    require(p.functions.eglChooseConfig(p.display, config_attributes, &config, 1, &count) != 0 && count > 0,
            "No suitable ANGLE GLES2 configuration");
    p.surface =
        p.functions.eglCreateWindowSurface(p.display, config, window, static_cast<const I *>(nullptr));
    const I context_attributes[] = {0x3098, 2, 0x3038};
    p.context =
        p.functions.eglCreateContext(p.display, config, static_cast<P>(nullptr), context_attributes);
    require(p.surface && p.context, "ANGLE surface/context creation failed");
    require(p.functions.eglMakeCurrent(p.display, p.surface, p.surface, p.context) != 0,
            "Cannot activate ANGLE context");
    p.functions.eglSwapInterval(p.display, 1);
    auto version = reinterpret_cast<const char *>(p.functions.glGetString(U{0x1f02}));
    auto vendor = reinterpret_cast<const char *>(p.functions.glGetString(U{0x1f00}));
    auto renderer = reinterpret_cast<const char *>(p.functions.glGetString(U{0x1f01}));
    p.version = version ? version : "";
    p.vendor = vendor ? vendor : "";
    p.renderer = renderer ? renderer : "";
    auto extensions = reinterpret_cast<const char *>(p.functions.glGetString(U{0x1f03}));
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
            p.functions.eglMakeCurrent(p.display, static_cast<P>(nullptr), static_cast<P>(nullptr),
                                       static_cast<P>(nullptr));
            if (p.context)
                p.functions.eglDestroyContext(p.display, p.context);
            if (p.surface)
                p.functions.eglDestroySurface(p.display, p.surface);
            p.functions.eglTerminate(p.display);
        } catch (...) {
        }
    }
}
void Graphics::present() {
    auto &p = *impl_;
    require(p.functions.eglSwapBuffers(p.display, p.surface) != 0, "EGL presentation failed");
}
void Graphics::resize(int w, int h) {
    require(w > 0 && h > 0, "Invalid desktop drawable dimensions");
    impl_->width = w;
    impl_->height = h;
}
void Graphics::set_guest_size(int width, int height) {
    auto &p = *impl_;
    require(width > 0 && height > 0 && std::uint64_t(width) * height <= 16'777'216,
            "Invalid Android surface dimensions");
    if (p.guest_framebuffer && width == p.guest_width && height == p.guest_height)
        return;
    require(p.version.starts_with("OpenGL ES 3."), "Desktop scaling requires ANGLE OpenGL ES 3");
    I texture{}, renderbuffer{}, framebuffer{}, read_framebuffer{};
    p.functions.glGetIntegerv(U{0x8069}, &texture);
    p.functions.glGetIntegerv(U{0x8ca7}, &renderbuffer);
    p.functions.glGetIntegerv(U{0x8ca6}, &framebuffer);
    p.functions.glGetIntegerv(U{0x8caa}, &read_framebuffer);
    if (!p.guest_texture)
        p.functions.glGenTextures(1, &p.guest_texture);
    p.functions.glBindTexture(U{0x0de1}, p.guest_texture);
    p.functions.glTexParameteri(U{0x0de1}, U{0x2801}, I{0x2601});
    p.functions.glTexParameteri(U{0x0de1}, U{0x2800}, I{0x2601});
    p.functions.glTexParameteri(U{0x0de1}, U{0x2802}, I{0x812f});
    p.functions.glTexParameteri(U{0x0de1}, U{0x2803}, I{0x812f});
    p.functions.glTexImage2D(U{0x0de1}, 0, I{0x8058}, width, height, 0, U{0x1908}, U{0x1401},
                             static_cast<const void *>(nullptr));
    if (!p.guest_depth)
        p.functions.glGenRenderbuffers(1, &p.guest_depth);
    p.functions.glBindRenderbuffer(U{0x8d41}, p.guest_depth);
    p.functions.glRenderbufferStorage(U{0x8d41}, U{0x88f0}, width, height);
    if (!p.guest_framebuffer)
        p.functions.glGenFramebuffers(1, &p.guest_framebuffer);
    p.functions.glBindFramebuffer(U{0x8d40}, p.guest_framebuffer);
    p.functions.glFramebufferTexture2D(U{0x8d40}, U{0x8ce0}, U{0x0de1}, p.guest_texture, 0);
    p.functions.glFramebufferRenderbuffer(U{0x8d40}, U{0x821a}, U{0x8d41}, p.guest_depth);
    require(p.functions.glCheckFramebufferStatus(U{0x8d40}) == 0x8cd5,
            "Cannot allocate the stable Android surface");
    p.guest_width = width;
    p.guest_height = height;
    p.functions.glBindTexture(U{0x0de1}, static_cast<U>(texture));
    p.functions.glBindRenderbuffer(U{0x8d41}, static_cast<U>(renderbuffer));
    p.functions.glBindFramebuffer(U{0x8d40},
                                  framebuffer ? static_cast<U>(framebuffer) : p.guest_framebuffer);
    p.functions.glBindFramebuffer(U{0x8ca8}, read_framebuffer ? static_cast<U>(read_framebuffer)
                                                              : p.guest_framebuffer);
    p.functions.glViewport(0, 0, width, height);
}
void Graphics::vsync(bool enabled) {
    require(impl_->functions.eglSwapInterval(impl_->display, enabled ? 1 : 0) != 0,
            "Cannot configure EGL vertical synchronization");
}
void Graphics::finish_frame() {
    auto &p = *impl_;
    if (!p.guest_framebuffer)
        return;
    // Presentation must not leak framebuffer, clear or scissor state into the guest.
    I draw{}, read{};
    std::array<B, 4> mask{};
    std::array<F, 4> color{};
    auto scissor = p.functions.glIsEnabled(U{0x0c11});
    p.functions.glGetIntegerv(U{0x8ca6}, &draw);
    p.functions.glGetIntegerv(U{0x8caa}, &read);
    p.functions.glGetBooleanv(U{0x0c23}, mask.data());
    p.functions.glGetFloatv(U{0x0c22}, color.data());
    p.functions.glBindFramebuffer(U{0x8ca9}, U{0});
    p.functions.glDisable(U{0x0c11});
    p.functions.glColorMask(B{1}, B{1}, B{1}, B{1});
    p.functions.glClearColor(F{0}, F{0}, F{0}, F{1});
    p.functions.glClear(U{0x4000});
    p.functions.glBindFramebuffer(U{0x8ca8}, p.guest_framebuffer);
    auto viewport = host::fit_viewport(p.width, p.height, p.guest_width, p.guest_height);
    auto bottom = p.height - viewport.y - viewport.height;
    p.functions.glBlitFramebuffer(0, 0, p.guest_width, p.guest_height, viewport.x, bottom,
                                  viewport.x + viewport.width, bottom + viewport.height, U{0x4000},
                                  U{0x2601});
    p.functions.glBindFramebuffer(U{0x8ca9}, static_cast<U>(draw));
    p.functions.glBindFramebuffer(U{0x8ca8}, static_cast<U>(read));
    p.functions.glClearColor(color[0], color[1], color[2], color[3]);
    p.functions.glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor)
        p.functions.glEnable(U{0x0c11});
}
Bytes Graphics::read_rgba(int width, int height) {
    require(width > 0 && height > 0 && std::uint64_t(width) * height <= 16'777'216,
            "GLES capture dimensions are invalid");
    Bytes pixels(static_cast<size_t>(width) * height * 4);
    I read{};
    if (impl_->guest_framebuffer) {
        impl_->functions.glGetIntegerv(U{0x8caa}, &read);
        impl_->functions.glBindFramebuffer(U{0x8ca8}, U{0});
    }
    impl_->functions.glReadPixels(0, 0, width, height, U{0x1908}, U{0x1401},
                                  static_cast<void *>(pixels.data()));
    if (impl_->guest_framebuffer)
        impl_->functions.glBindFramebuffer(U{0x8ca8}, static_cast<U>(read));
    require(impl_->functions.glGetError() == 0, "GLES capture failed");
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
            {"guest_width", p.guest_width},
            {"guest_height", p.guest_height},
            {"guest_viewports", p.viewports},
            {"directory", utf8(p.directory)}};
}
} // namespace acp::android
