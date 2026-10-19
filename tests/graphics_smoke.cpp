#include <acp/android.hpp>

#include <iostream>
int main() {
    using namespace acp;
    try {
        auto root = fs::current_path() / "out/graphics-smoke";
        android::Runtime r({"org.example.graphics", "1.0", "en_US", 1, root / "data", {}, {}});
        r.host->open_window("Android compatibility GLES bridge validation", 128, 128);
        auto geometry = r.host->describe_window();
        require(geometry.at("client_origin_in_window")[1].get<int>() > 0 &&
                    geometry.at("render_origin_in_client") == Json::array({0, 0}) &&
                    geometry.at("render_size") == geometry.at("client_size") &&
                    r.host->native_window() != r.host->render_window(),
                "EGL must use a separate child covering only the client area below the title bar");
        r.graphics->open(r.host->render_window(), {});
        r.graphics->resize(128, 128);
        auto call = [&](std::string_view name, std::initializer_list<std::uint64_t> args = {}) {
            return r.cpu.call(r.linker.symbol(name), std::span(args.begin(), args.size()));
        };
        auto shader = [&](unsigned type, std::string_view source) {
            auto id = call("glCreateShader", {type});
            auto text = r.memory.string(source), array = r.memory.allocate(8);
            r.memory.store(array, text);
            call("glShaderSource", {id, 1, array, 0});
            call("glCompileShader", {id});
            auto status = r.memory.allocate(4);
            call("glGetShaderiv", {id, 0x8b81, status});
            require(r.memory.load<int>(status) != 0, "GLES shader compilation failed");
            return id;
        };
        auto vertex =
            shader(0x8b31, "attribute vec2 aPosition; void main(){gl_Position=vec4(aPosition,0.0,1.0);}");
        auto fragment =
            shader(0x8b30, "precision mediump float; void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}");
        auto program = call("glCreateProgram");
        call("glAttachShader", {program, vertex});
        call("glAttachShader", {program, fragment});
        call("glBindAttribLocation", {program, 0, r.memory.string("aPosition")});
        call("glLinkProgram", {program});
        auto status = r.memory.allocate(4);
        call("glGetProgramiv", {program, 0x8b82, status});
        require(r.memory.load<int>(status) != 0, "GLES program link failed");
        call("glUseProgram", {program});
        const std::array<float, 6> positions{-.8f, -.8f, .8f, -.8f, 0, .8f};
        auto vertices = r.memory.allocate(sizeof(positions));
        r.memory.write(vertices, std::as_bytes(std::span(positions)));
        call("glVertexAttribPointer", {0, 2, 0x1406, 0, 0, vertices});
        call("glEnableVertexAttribArray", {0});
        call("glViewport", {0, 0, 128, 128});
        r.cpu.f32(0, 0);
        r.cpu.f32(1, 0);
        r.cpu.f32(2, 1);
        r.cpu.f32(3, 1);
        call("glClearColor");
        call("glClear", {0x4000});
        call("glDrawArrays", {4, 0, 3});
        auto pixels = r.graphics->read_rgba(128, 128);
        auto center = (64 * 128 + 64) * 4;
        require(pixels[center] == std::byte{255} && pixels[center + 1] == std::byte{} &&
                    pixels[center + 2] == std::byte{},
                "GLES triangle pixel validation failed");
        require(pixels[2] == std::byte{255}, "GLES clear pixel validation failed");
        write_png_rgba(root / "gles-triangle.png", 128, 128, pixels, true);
        auto version = r.memory.string(call("glGetString", {0x1f02}));
        auto extensions = r.memory.string(call("glGetString", {0x1f03}));
        auto texture_id = r.memory.allocate(4);
        auto format_count = r.memory.allocate(4);
        call("glGetIntegerv", {0x86a2, format_count});
        auto count = r.memory.load<int>(format_count);
        require(count > 0 && count < 4096, "Invalid compressed format inventory");
        auto formats = r.memory.allocate(size_t(count) * 4);
        call("glGetIntegerv", {0x86a3, formats});
        bool contains_etc1 = false;
        for (int i = 0; i < count; ++i)
            contains_etc1 |= r.memory.load<unsigned>(formats + i * 4) == 0x8d64;
        require(contains_etc1, "ETC1 is missing from advertised compressed formats");
        call("glGenTextures", {1, texture_id});
        auto texture = r.memory.load<unsigned>(texture_id);
        call("glBindTexture", {0x0de1, texture});
        call("glTexParameteri", {0x0de1, 0x2801, 0x2600});
        call("glTexParameteri", {0x0de1, 0x2800, 0x2600});
        auto block = r.memory.allocate(8);
        call("glCompressedTexImage2D", {0x0de1, 0, 0x8d64, 4, 4, 0, 8, block});
        require(call("glGetError") == 0, "ETC1 texture upload failed");
        auto texture_fragment = shader(0x8b30, "precision mediump float; uniform sampler2D image; void "
                                               "main(){gl_FragColor=texture2D(image,vec2(0.5));}");
        auto texture_program = call("glCreateProgram");
        call("glAttachShader", {texture_program, vertex});
        call("glAttachShader", {texture_program, texture_fragment});
        call("glBindAttribLocation", {texture_program, 0, r.memory.string("aPosition")});
        call("glLinkProgram", {texture_program});
        call("glGetProgramiv", {texture_program, 0x8b82, status});
        require(r.memory.load<int>(status) != 0, "ETC1 shader link failed");
        call("glUseProgram", {texture_program});
        auto uniform = call("glGetUniformLocation", {texture_program, r.memory.string("image")});
        call("glUniform1i", {uniform, 0});
        call("glDrawArrays", {4, 0, 3});
        auto etc1_pixels = r.graphics->read_rgba(128, 128);
        for (int channel = 0; channel < 3; ++channel)
            require(std::to_integer<unsigned>(etc1_pixels[center + channel]) >= 1 &&
                        std::to_integer<unsigned>(etc1_pixels[center + channel]) <= 3,
                    "ETC1 decoded pixel mismatch");
        // Exercise the same fixed-surface presentation path as the real game.
        r.graphics->set_guest_size(128, 72);
        r.graphics->resize(128, 128);
        r.cpu.f32(0, 1);
        r.cpu.f32(1, 0);
        r.cpu.f32(2, 0);
        r.cpu.f32(3, 1);
        call("glClearColor");
        call("glClear", {0x4000});
        call("glViewport", {5, 7, 110, 60});
        call("glScissor", {8, 9, 10, 11});
        call("glEnable", {0x0c11});
        call("glColorMask", {0, 1, 0, 1});
        r.graphics->finish_frame();
        auto scaled = r.graphics->read_rgba(128, 128);
        require(scaled[center] == std::byte{255} && scaled[center + 1] == std::byte{0} &&
                    scaled[center + 2] == std::byte{0} && scaled[0] == std::byte{0} &&
                    scaled[1] == std::byte{0} && scaled[2] == std::byte{0},
                "Fixed Android surface must fill the fitted viewport with black letterbox bars");
        auto state = r.memory.allocate(16);
        call("glGetIntegerv", {0x8ca6, state});
        require(r.memory.load<unsigned>(state) == 0,
                "Host surface is invisible to guest framebuffer queries");
        call("glGetIntegerv", {0x0ba2, state});
        require(r.memory.load<int>(state) == 5 && r.memory.load<int>(state + 4) == 7 &&
                    r.memory.load<int>(state + 8) == 110 && r.memory.load<int>(state + 12) == 60,
                "Presentation preserves guest viewport state");
        call("glGetIntegerv", {0x0c10, state});
        require(r.memory.load<int>(state) == 8 && r.memory.load<int>(state + 12) == 11,
                "Presentation preserves guest scissor state");
        call("glBindFramebuffer", {0x8d40, 0});
        require(call("glCheckFramebufferStatus", {0x8d40}) == 0x8cd5 && call("glGetError") == 0,
                "Binding guest framebuffer zero restores the complete Android surface");
        // The game also draws to its own FBOs. Presentation must keep those bindings
        // intact and must always present the Android default surface, not that FBO.
        auto offscreen_id = r.memory.allocate(4);
        call("glGenTextures", {1, texture_id});
        call("glBindTexture", {0x0de1, r.memory.load<unsigned>(texture_id)});
        call("glTexImage2D", {0x0de1, 0, 0x1908, 4, 4, 0, 0x1908, 0x1401, 0});
        call("glGenFramebuffers", {1, offscreen_id});
        auto offscreen = r.memory.load<unsigned>(offscreen_id);
        call("glBindFramebuffer", {0x8d40, offscreen});
        call("glFramebufferTexture2D", {0x8d40, 0x8ce0, 0x0de1, r.memory.load<unsigned>(texture_id), 0});
        require(call("glCheckFramebufferStatus", {0x8d40}) == 0x8cd5, "Guest offscreen FBO is complete");
        call("glDisable", {0x0c11});
        call("glColorMask", {1, 1, 1, 1});
        r.cpu.f32(0, 0);
        r.cpu.f32(1, 1);
        r.cpu.f32(2, 0);
        r.cpu.f32(3, 1);
        call("glClearColor");
        call("glClear", {0x4000});
        r.graphics->finish_frame();
        auto offscreen_present = r.graphics->read_rgba(128, 128);
        require(offscreen_present[center] == std::byte{255} &&
                    offscreen_present[center + 1] == std::byte{0},
                "An active guest offscreen FBO must not replace the presented Android surface");
        for (auto binding : {0x8ca6, 0x8caa}) {
            call("glGetIntegerv", {static_cast<unsigned>(binding), state});
            require(r.memory.load<unsigned>(state) == offscreen,
                    "Presentation and readback preserve guest draw/read framebuffer bindings");
        }
        call("glBindFramebuffer", {0x8d40, 0});
        require(call("glGetError") == 0, "Offscreen presentation has no GL errors");
        write_png_rgba(root / "letterbox.png", 128, 128, scaled, true);
        r.graphics->set_guest_size(64, 96);
        call("glDisable", {0x0c11});
        r.cpu.f32(0, 0);
        r.cpu.f32(1, 0);
        r.cpu.f32(2, 1);
        r.cpu.f32(3, 1);
        call("glClearColor");
        call("glClear", {0x4000});
        r.graphics->finish_frame();
        auto resized = r.graphics->read_rgba(128, 128);
        auto upper_center = (120 * 128 + 64) * 4;
        require(resized[center + 2] == std::byte{255} && resized[upper_center + 2] == std::byte{255} &&
                    resized[0] == std::byte{0} && resized[2] == std::byte{0},
                "Reallocating a live Android surface must render its full new height with correct "
                "pillarboxing");
        call("glGetIntegerv", {0x0ba2, state});
        require(r.memory.load<int>(state + 8) == 64 && r.memory.load<int>(state + 12) == 96 &&
                    call("glGetError") == 0,
                "Live resolution changes update the actual guest viewport");
        write_png_rgba(root / "native-resize.png", 128, 128, resized, true);
        Json report{
            {"result", "passed"},
            {"version", version},
            {"etc1_advertised", extensions.find("GL_OES_compressed_ETC1_RGB8_texture") != extensions.npos},
            {"validated", "GLES shader compilation, linking, guest client arrays, draw, pixel readback, "
                          "ETC1 decoding, client-area placement, render-buffer resizing, letterboxing "
                          "and GL state preservation"}};
        write_json(root / "report.json", report);
        r.graphics->present();
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
