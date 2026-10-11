#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"
#include <dolphin/os/OSRtc.h>
#include "dusk/settings.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

union SDL_Event;

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);
// The hook helper requires a mutable reference in its trampoline; the game's
// parameter is const, and this hook only reads it. Reference ABI is identical.
DEFINE_HOOK_SYMBOL("dusk::ui::handle_event", void(SDL_Event&), UiEvent);

namespace game_symbols {
using GetSettings = dusk::UserSettings& (*)();
using HasSubscribers = bool (*)(const dusk::config::ConfigVarBase*);
using NotifyChanged = void (*)(dusk::config::ConfigVarBase*, const void*);

GetSettings get_settings = nullptr;
HasSubscribers has_subscribers = nullptr;
NotifyChanged notify_changed = nullptr;

template <typename Fn>
ModResult resolve(const char* name, Fn& out) {
    void* address = nullptr;
    const ModResult result = svc_hook->resolve(mod_ctx, name, &address, nullptr);
    if (result == MOD_OK) out = reinterpret_cast<Fn>(address);
    return result;
}
}

// These internal functions are present in the game but absent from the SDK's
// version-independent macOS link stub. Resolve them through Dusklight instead.
namespace dusk {
UserSettings& getSettings() {
    return game_symbols::get_settings();
}

namespace config {
bool ConfigVarBase::has_subscribers() const {
    return game_symbols::has_subscribers(this);
}

void ConfigVarBase::notify_changed(const void* previous) {
    game_symbols::notify_changed(this, previous);
}
}
}

namespace {
bool muted = false;
bool had_override = false;
int original_override = 0;
ConfigVarHandle binding_var = 0;
UiElementHandle binding_text = 0;
std::int32_t bound_scancode = 64;
bool capturing = false;

// SDL 3.2 keyboard event prefix. We only read these fixed ABI fields, avoiding
// a separate SDL link dependency in a native mod.
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
static_assert(offsetof(KeyboardEvent, scancode) == 24);
static_assert(offsetof(KeyboardEvent, repeat) == 37);
constexpr std::uint32_t key_down = 0x300;
constexpr std::int32_t f7 = 64;
constexpr std::int32_t escape = 41;

std::string binding_name(std::int32_t code) {
    if (code >= 4 && code <= 29) return std::string(1, static_cast<char>('A' + code - 4));
    if (code >= 30 && code <= 38) return std::string(1, static_cast<char>('1' + code - 30));
    if (code == 39) return "0";
    if (code >= 58 && code <= 69) return "F" + std::to_string(code - 57);
    switch (code) {
    case 40: return "Enter";
    case 42: return "Backspace";
    case 43: return "Tab";
    case 44: return "Space";
    case 79: return "Right Arrow";
    case 80: return "Left Arrow";
    case 81: return "Down Arrow";
    case 82: return "Up Arrow";
    default: return "Scancode " + std::to_string(code);
    }
}

std::string binding_status() {
    if (capturing) return "Press a key to bind it. Esc cancels.";
    return "Current key: " + binding_name(bound_scancode);
}

void begin_capture(ModContext*, void*) {
    capturing = true;
}

ModResult build_panel(ModContext* ctx, UiElementHandle panel, void*, ModError*) {
    binding_text = 0;
    ModResult result = svc_ui->pane_add_text(ctx, panel, binding_status().c_str(), &binding_text);
    if (result != MOD_OK) return result;

    UiControlDesc button = UI_CONTROL_DESC_INIT;
    button.kind = UI_CONTROL_BUTTON;
    button.label = "Change music mute key";
    button.on_pressed = begin_capture;
    result = svc_ui->pane_add_control(ctx, panel, &button, nullptr);
    if (result != MOD_OK) return result;
    return svc_ui->pane_add_text(ctx, panel,
        "The selected key toggles music while playing. Press Esc during capture to keep the current key.",
        nullptr);
}

ModResult update_panel(ModContext* ctx, void*, ModError*) {
    if (binding_text == 0) return MOD_OK;
    return svc_ui->elem_set_text(ctx, binding_text, binding_status().c_str());
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

HookAction on_ui_event(ModContext*, void* args, void*, void*) {
    const auto& event = mods::arg_ref<SDL_Event&>(args, 0);
    KeyboardEvent key{};
    std::memcpy(&key, &event, sizeof(key));
    if (key.type == key_down && !key.repeat) {
        if (capturing) {
            if (key.scancode != escape && key.scancode > 0 && key.scancode < 512) {
                if (svc_config->set_int(mod_ctx, binding_var, key.scancode) == MOD_OK) {
                    bound_scancode = key.scancode;
                    svc_log->info(mod_ctx, "Music mute key changed");
                }
            }
            capturing = false;
            return HOOK_SKIP_ORIGINAL;
        } else if (key.scancode == bound_scancode) {
            bool ui_visible = false;
            if (svc_ui->is_any_document_visible(mod_ctx, &ui_visible) == MOD_OK && !ui_visible) {
                if (muted) unmute();
                else mute();
            }
        }
    }
    return HOOK_CONTINUE;
}
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError* error) {
    muted = false;
    capturing = false;
    ModResult result = game_symbols::resolve("dusk::getSettings", game_symbols::get_settings);
    if (result == MOD_OK) {
        result = game_symbols::resolve(
            "dusk::config::ConfigVarBase::has_subscribers", game_symbols::has_subscribers);
    }
    if (result == MOD_OK) {
        result = game_symbols::resolve(
            "dusk::config::ConfigVarBase::notify_changed", game_symbols::notify_changed);
    }
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not resolve Dusklight music settings");
    }

    ConfigVarDesc binding = CONFIG_VAR_DESC_INIT;
    binding.name = "muteKey";
    binding.type = CONFIG_VAR_INT;
    binding.default_int = f7;
    result = svc_config->register_var(mod_ctx, &binding, &binding_var);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not register mute key setting");
    }
    std::int64_t saved_key = f7;
    result = svc_config->get_int(mod_ctx, binding_var, &saved_key);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not read mute key setting");
    }
    bound_scancode = saved_key > 0 && saved_key < 512 ? static_cast<std::int32_t>(saved_key) : f7;

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_panel;
    panel.update = update_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panel);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not register mod settings panel");
    }
    result = mods::hook::add_pre<UiEvent>(on_ui_event);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not hook Dusklight keyboard events");
    }
    svc_log->info(mod_ctx, "Music mute hotkey ready");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    // A settings change can clear our temporary override. Keep the music
    // muted until the bound key is pressed again, without saving zero to config.json.
    if (muted) {
        auto& volume = dusk::getSettings().audio.mainMusicVolume;
        if (volume.getValue() != 0) volume.setOverrideValue(0);
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    unmute();
    mods::hook::uninstall<UiEvent>();
    return MOD_OK;
}
}
