#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/ui.h"
#include <dolphin/os/OSRtc.h>
#include <dolphin/pad.h>
#include "dusk/action_bindings.h"
#include "dusk/settings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

union SDL_Event;

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(UiService, svc_ui);
// The hook helper requires mutable references. These hooks only read their
// const event and Pane arguments; references have the same calling ABI.
DEFINE_HOOK_SYMBOL("dusk::ui::handle_event", void(SDL_Event&), UiEvent);
// Page::Actions is 5 in Dusklight v2.0.1. The private enum and Pane reference
// are represented by their ABI-equivalent int and pointer here.
DEFINE_HOOK_SYMBOL("dusk::ui::ControllerConfigWindow::render_page",
    void(void*, void*, int, int), ControllerPage);

namespace game_symbols {
using GetSettings = dusk::UserSettings& (*)();
using HasSubscribers = bool (*)(const dusk::config::ConfigVarBase*);
using NotifyChanged = void (*)(dusk::config::ConfigVarBase*, const void*);
using Register = void (*)(dusk::config::ConfigVarBase&);
using Unregister = void (*)(dusk::config::ConfigVarBase&);
using GetActionBinds = dusk::ActionBindsMap& (*)();
using GetGamepadId = std::uint32_t (*)(SDL_Gamepad*);

GetSettings get_settings = nullptr;
HasSubscribers has_subscribers = nullptr;
NotifyChanged notify_changed = nullptr;
Register register_var = nullptr;
Unregister unregister_var = nullptr;
GetActionBinds get_action_binds = nullptr;
GetGamepadId get_gamepad_id = nullptr;
const dusk::config::ConfigImplBase* int_impl = nullptr;

template <typename Fn>
ModResult resolve(const char* name, Fn& out) {
    void* address = nullptr;
    const ModResult result = svc_hook->resolve(mod_ctx, name, &address, nullptr);
    if (result == MOD_OK) out = reinterpret_cast<Fn>(address);
    return result;
}
}

// The game does not expose dynamic action registration to mods. Use its native
// ConfigVar<int> layout for the controller menu's existing binding control.
namespace dusk {
UserSettings& getSettings() {
    return game_symbols::get_settings();
}

namespace config {
ConfigVarBase::ConfigVarBase(std::string name, const ConfigImplBase* impl)
    : name(std::move(name)), registered(false), layer(ConfigVarLayer::Default), impl(impl) {}
ConfigVarBase::~ConfigVarBase() = default;
const char* ConfigVarBase::getName() const noexcept { return name.c_str(); }
const ConfigImplBase* ConfigVarBase::getImpl() const noexcept { return impl; }
bool ConfigVarBase::has_subscribers() const {
    return game_symbols::has_subscribers(this);
}
void ConfigVarBase::notify_changed(const void* previous) {
    game_symbols::notify_changed(this, previous);
}
template <>
const ConfigImplBase* GetConfigImpl<int>() {
    return game_symbols::int_impl;
}
}
}

namespace {
using Bindings = std::array<dusk::config::ActionBindConfigVar, PAD_CHANMAX>;
constexpr std::int32_t f7 = 64;
constexpr std::uint32_t key_down = 0x300;
constexpr std::uint32_t gamepad_button_down = 0x651;
constexpr int actions_page = 5;

bool muted = false;
bool had_override = false;
int original_override = 0;
Bindings* keyboard_bindings = nullptr;
Bindings* gamepad_bindings = nullptr;

// Fixed SDL 3.2 event prefixes, so the mod needs no separate SDL link.
struct KeyboardEvent {
    std::uint32_t type;
    std::uint32_t reserved;
    std::uint64_t timestamp;
    std::uint32_t window_id;
    std::uint32_t which;
    std::int32_t scancode;
    std::uint32_t key;
    std::uint16_t modifiers;
    std::uint16_t raw;
    bool down;
    bool repeat;
};
struct GamepadButtonEvent {
    std::uint32_t type;
    std::uint32_t reserved;
    std::uint64_t timestamp;
    std::uint32_t which;
    std::uint8_t button;
};
static_assert(offsetof(KeyboardEvent, scancode) == 24);
static_assert(offsetof(KeyboardEvent, repeat) == 37);
static_assert(offsetof(GamepadButtonEvent, button) == 20);

bool keyboard_active(int port) {
    std::uint32_t count = 0;
    return PADGetKeyButtonBindings(static_cast<std::uint32_t>(port), &count) != nullptr;
}

std::uint32_t selected_gamepad_id(int port) {
    const s32 index = PADGetIndexForPort(static_cast<std::uint32_t>(port));
    if (index < 0) return 0;
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<std::uint32_t>(index));
    return pad == nullptr ? 0 : game_symbols::get_gamepad_id(pad);
}

void release_bindings() {
    if (keyboard_bindings != nullptr) {
        for (auto& binding : *keyboard_bindings) game_symbols::unregister_var(binding);
        delete keyboard_bindings;
        keyboard_bindings = nullptr;
    }
    if (gamepad_bindings != nullptr) {
        for (auto& binding : *gamepad_bindings) game_symbols::unregister_var(binding);
        delete gamepad_bindings;
        gamepad_bindings = nullptr;
    }
}

void unmute() {
    if (!muted) return;

    auto& volume = dusk::getSettings().audio.mainMusicVolume;
    if (volume.getLayer() == dusk::config::ConfigVarLayer::Override && volume.getValue() == 0) {
        volume.clearOverride();
        if (had_override) volume.setOverrideValue(original_override);
    }
    muted = false;
    svc_log->info(mod_ctx, "Music unmuted");
}

void mute() {
    auto& volume = dusk::getSettings().audio.mainMusicVolume;
    had_override = volume.getLayer() == dusk::config::ConfigVarLayer::Override;
    if (had_override) original_override = volume.getValue();
    volume.setOverrideValue(0);
    muted = true;
    svc_log->info(mod_ctx, "Music muted");
}

HookAction add_mute_action(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 3) == actions_page) {
        const int port = mods::arg<int>(args, 2);
        auto* bindings = keyboard_active(port) ? keyboard_bindings : gamepad_bindings;
        game_symbols::get_action_binds().emplace(
            static_cast<dusk::ActionBinds>(dusk::ActionBinds::COUNT),
            dusk::ActionBindData{bindings, "Mute Music"});
    }
    return HOOK_CONTINUE;
}

void remove_mute_action(ModContext*, void* args, void*, void*) {
    if (mods::arg<int>(args, 3) == actions_page) {
        game_symbols::get_action_binds().erase(
            static_cast<dusk::ActionBinds>(dusk::ActionBinds::COUNT));
    }
}

HookAction on_ui_event(ModContext*, void* args, void*, void*) {
    const auto& event = mods::arg_ref<SDL_Event&>(args, 0);
    std::uint32_t type = 0;
    std::memcpy(&type, &event, sizeof(type));

    bool pressed = false;
    if (type == key_down) {
        KeyboardEvent key{};
        std::memcpy(&key, &event, sizeof(key));
        if (!key.repeat) {
            for (int port = 0; port < PAD_CHANMAX; ++port) {
                if (key.scancode == (*keyboard_bindings)[port].getValue()) {
                    pressed = true;
                    break;
                }
            }
        }
    } else if (type == gamepad_button_down) {
        GamepadButtonEvent button{};
        std::memcpy(&button, &event, sizeof(button));
        for (int port = 0; port < PAD_CHANMAX; ++port) {
            const int bound = (*gamepad_bindings)[port].getValue();
            const std::uint32_t selected = selected_gamepad_id(port);
            if (bound != PAD_NATIVE_BUTTON_INVALID && selected != 0 &&
                button.which == selected && static_cast<int>(button.button) == bound)
            {
                pressed = true;
                break;
            }
        }
    }

    if (pressed) {
        bool ui_visible = false;
        if (svc_ui->is_any_document_visible(mod_ctx, &ui_visible) == MOD_OK && !ui_visible) {
            if (muted) unmute();
            else mute();
        }
    }
    return HOOK_CONTINUE;
}
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError* error) {
    muted = false;
    ModResult result = game_symbols::resolve("dusk::getSettings", game_symbols::get_settings);
    if (result == MOD_OK) result = game_symbols::resolve(
        "dusk::config::ConfigVarBase::has_subscribers", game_symbols::has_subscribers);
    if (result == MOD_OK) result = game_symbols::resolve(
        "dusk::config::ConfigVarBase::notify_changed", game_symbols::notify_changed);
    // Register has template overloads; use this exact ConfigVarBase& symbol.
    if (result == MOD_OK) result = game_symbols::resolve(
        "_ZN4dusk6config8RegisterERNS0_13ConfigVarBaseE", game_symbols::register_var);
    if (result == MOD_OK) result = game_symbols::resolve(
        "dusk::config::unregister", game_symbols::unregister_var);
    if (result == MOD_OK) result = game_symbols::resolve(
        "dusk::getActionBinds", game_symbols::get_action_binds);
    if (result == MOD_OK) result = game_symbols::resolve(
        "SDL_GetGamepadID", game_symbols::get_gamepad_id);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not resolve Dusklight controller bindings");
    }

    game_symbols::int_impl = dusk::getSettings().audio.mainMusicVolume.getImpl();
    keyboard_bindings = new Bindings{{
        {"mods.muteMusic.keyboard_port0", f7},
        {"mods.muteMusic.keyboard_port1", PAD_NATIVE_BUTTON_INVALID},
        {"mods.muteMusic.keyboard_port2", PAD_NATIVE_BUTTON_INVALID},
        {"mods.muteMusic.keyboard_port3", PAD_NATIVE_BUTTON_INVALID},
    }};
    gamepad_bindings = new Bindings{{
        {"mods.muteMusic.gamepad_port0", PAD_NATIVE_BUTTON_INVALID},
        {"mods.muteMusic.gamepad_port1", PAD_NATIVE_BUTTON_INVALID},
        {"mods.muteMusic.gamepad_port2", PAD_NATIVE_BUTTON_INVALID},
        {"mods.muteMusic.gamepad_port3", PAD_NATIVE_BUTTON_INVALID},
    }};
    for (auto& binding : *keyboard_bindings) game_symbols::register_var(binding);
    for (auto& binding : *gamepad_bindings) game_symbols::register_var(binding);

    result = mods::hook::add_pre<ControllerPage>(add_mute_action);
    if (result == MOD_OK) result = mods::hook::add_post<ControllerPage>(remove_mute_action);
    if (result != MOD_OK) {
        mods::hook::uninstall<ControllerPage>();
        release_bindings();
        return mods::set_error(error, result, "Could not extend Dusklight controller menu");
    }
    result = mods::hook::add_pre<UiEvent>(on_ui_event);
    if (result != MOD_OK) {
        mods::hook::uninstall<ControllerPage>();
        release_bindings();
        return mods::set_error(error, result, "Could not hook Dusklight input events");
    }
    svc_log->info(mod_ctx, "Music mute binding ready in Controller Settings");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    // A settings change can clear our temporary override. Keep the music
    // muted until the bound input is pressed again, without saving zero.
    if (muted) {
        auto& volume = dusk::getSettings().audio.mainMusicVolume;
        if (volume.getValue() != 0) volume.setOverrideValue(0);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    unmute();
    mods::hook::uninstall<UiEvent>();
    mods::hook::uninstall<ControllerPage>();
    release_bindings();
    return MOD_OK;
}
}
