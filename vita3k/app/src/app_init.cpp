// Vita3K emulator project
// Copyright (C) 2018 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <app/functions.h>

#include <audio/functions.h>
#include <config/functions.h>
#include <config/version.h>
#include <gui/imgui_impl_sdl.h>
#include <host/state.h>
#include <io/functions.h>
#include <mem/mem.h>
#include <nids/functions.h>
#include <renderer/functions.h>
#include <rtc/rtc.h>
#include <util/fs.h>
#include <util/lock_and_find.h>
#include <util/log.h>
#include <util/string_utils.h>

#if USE_DISCORD
#include <app/discord.h>
#endif

#ifdef USE_GDBSTUB
#include <gdbstub/functions.h>
#endif

#ifdef USE_VULKAN
#include <renderer/vulkan/functions.h>
#include <util/string_utils.h>
#endif

#ifdef WIN32
#include <ShellScalingAPI.h>
#endif

#include <SDL_video.h>
#include <SDL_vulkan.h>

namespace app {
void update_viewport(HostState &state) {
    int w = 0;
    int h = 0;

    switch (state.renderer->current_backend) {
    case renderer::Backend::OpenGL:
        SDL_GL_GetDrawableSize(state.window.get(), &w, &h);
        break;
#ifdef USE_VULKAN
    case renderer::Backend::Vulkan:
        SDL_Vulkan_GetDrawableSize(state.window.get(), &w, &h);
        break;
#endif
    default:
        LOG_ERROR("Unimplemented backend render: {}.", static_cast<int>(state.renderer->current_backend));
        break;
    }

    state.drawable_size.x = w;
    state.drawable_size.y = h;

    if (h > 0) {
        const float window_aspect = static_cast<float>(w) / h;
        const float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
        if (window_aspect > vita_aspect) {
            // Window is wide. Pin top and bottom.
            state.viewport_size.x = h * vita_aspect;
            state.viewport_size.y = static_cast<SceFloat>(h);
            state.viewport_pos.x = (w - state.viewport_size.x) / 2;
            state.viewport_pos.y = 0;
        } else {
            // Window is tall. Pin left and right.
            state.viewport_size.x = static_cast<SceFloat>(w);
            state.viewport_size.y = w / vita_aspect;
            state.viewport_pos.x = 0;
            state.viewport_pos.y = (h - state.viewport_size.y) / 2;
        }
    } else {
        state.viewport_pos.x = 0;
        state.viewport_pos.y = 0;
        state.viewport_size.x = 0;
        state.viewport_size.y = 0;
    }
}

bool init(HostState &state, Config &cfg, const Root &root_paths, CPUDepInject inject) {
    const ResumeAudioThread resume_thread = [&state](SceUID thread_id) {
        const auto thread = lock_and_find(thread_id, state.kernel.threads, state.kernel.mutex);
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->to_do == ThreadToDo::wait) {
            thread->to_do = ThreadToDo::run;
        }
        thread->something_to_do.notify_all();
    };

    state.cfg = std::move(cfg);
    state.kernel.wait_for_debugger = state.cfg.wait_for_debugger;

    state.base_path = root_paths.get_base_path_string();
    state.default_path = root_paths.get_pref_path_string();

    // If configuration does not provide a preference path, use SDL's default
    if (state.cfg.pref_path == root_paths.get_pref_path() || state.cfg.pref_path.empty())
        state.pref_path = string_utils::utf_to_wide(root_paths.get_pref_path_string());
    else {
        if (state.cfg.pref_path.back() != '/')
            state.cfg.pref_path += '/';
        state.pref_path = string_utils::utf_to_wide(state.cfg.pref_path);
    }

#ifdef USE_VULKAN
    if (string_utils::toupper(state.cfg.backend_renderer) == "VULKAN")
        state.backend_renderer = renderer::Backend::Vulkan;
    else
#endif
        state.backend_renderer = renderer::Backend::OpenGL;

    int window_type = 0;
    switch (state.backend_renderer) {
    case renderer::Backend::OpenGL:
        window_type = SDL_WINDOW_OPENGL;
        break;
#ifdef USE_VULKAN
    case renderer::Backend::Vulkan:
        window_type = SDL_WINDOW_VULKAN;
        break;
#endif
    default:
        LOG_ERROR("Unimplemented backend render: {}.", state.cfg.backend_renderer);
        break;
    }

    if (cfg.fullscreen) {
        state.display.fullscreen = true;
        window_type |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
#ifdef WIN32
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    float ddpi, hdpi, vdpi;
    SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi);
    float dpiScale = ddpi / 96;
    state.window = WindowPtr(SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DEFAULT_RES_WIDTH * dpiScale, DEFAULT_RES_HEIGHT * dpiScale, window_type | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI), SDL_DestroyWindow);
#else
    state.window = WindowPtr(SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DEFAULT_RES_WIDTH, DEFAULT_RES_HEIGHT, window_type | SDL_WINDOW_RESIZABLE), SDL_DestroyWindow);
#endif

    if (!state.window) {
        LOG_ERROR("SDL failed to create window!");
        return false;
    }

    if (!init(state.mem)) {
        LOG_ERROR("Failed to initialize memory for emulator state!");
        return false;
    }

    if (!init(state.audio, resume_thread)) {
        LOG_WARN("Failed to init audio! Audio will not work.");
    }

    if (!init(state.io, state.base_path, state.pref_path, cfg.console)) {
        LOG_ERROR("Failed to initialize file system for the emulator!");
        return false;
    }

    if (!ngs::init(state.ngs, state.mem)) {
        LOG_ERROR("Failed to initialize ngs.");
        return false;
    }

#if USE_DISCORD
    if (discordrpc::init() && cfg.discord_rich_presence) {
        discordrpc::update_presence();
    }
#endif

    for (int i = 0; i < cfg.cpu_pool_size; ++i) {
        auto item = init_cpu(0, 0, 0, state.mem, inject);
        state.kernel.cpu_pool.add(std::move(item));
    }

    state.kernel.start_tick = { rtc_base_ticks() };
    state.kernel.base_tick = { rtc_base_ticks() };

    if (!cfg.console) {
        if (renderer::init(state.window.get(), state.renderer, state.backend_renderer)) {
            update_viewport(state);
            return true;
        } else {
            switch (state.backend_renderer) {
            case renderer::Backend::OpenGL:
                error_dialog("Could not create OpenGL context!\nDoes your GPU at least support OpenGL 4.1?", nullptr);
                break;
#ifdef USE_VULKAN
            case renderer::Backend::Vulkan:
                error_dialog("Could not create Vulkan context!");
                break;
#endif
            default:
                error_dialog(fmt::format("Unknown backend render: {}.", state.cfg.backend_renderer));
                break;
            }
            return false;
        }
    }

    return true;
}

void destroy(HostState &host, ImGui_State *imgui) {
    ImGui_ImplSdl_Shutdown(imgui);
#ifdef USE_VULKAN
    // I'm explicitly destroying VulkanState in app::destroy instead of a destructor because I want to ensure an order.
    // Objects in Vulkan should be destroyed in reverse order than they were created.
    if (host.renderer->current_backend == renderer::Backend::Vulkan) {
        renderer::vulkan::close(host.renderer);
    }
#endif

#ifdef USE_DISCORD
    discordrpc::shutdown();
#endif

#ifdef USE_GDBSTUB
    server_close(host);
#endif

    // There may be changes that made in the GUI, so we should save, again
    if (host.cfg.overwrite_config)
        config::serialize_config(host.cfg, host.cfg.config_path);
}

} // namespace app
