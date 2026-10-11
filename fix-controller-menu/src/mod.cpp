#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"

#include <dolphin/pad.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// The SDK deliberately does not include SDL headers. These are the stable SDL 3
// event prefix and gamepad event values used by Dusklight v2.0.1.
union alignas(8) SDL_Event {
    std::uint32_t type;
    std::uint8_t padding[128];
};
static_assert(sizeof(SDL_Event) == 128);
static_assert(alignof(SDL_Event) == 8);

DEFINE_MOD();
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(LogService, svc_log);
// The hook trampoline requires a mutable reference; the game's parameter is
// const and this callback only reads it. The reference ABI is identical.
DEFINE_HOOK_SYMBOL("dusk::ui::input::handle_event", void(SDL_Event&), MenuInputEvent);

namespace {
constexpr std::uint32_t gamepad_axis_motion = 0x650;
constexpr std::uint32_t gamepad_button_down = 0x651;
constexpr std::uint32_t gamepad_button_up = 0x652;
constexpr std::uint32_t window_focus_lost = 0x20f;

struct GamepadEventPrefix {
    std::uint32_t type;
    std::uint32_t reserved;
    std::uint64_t timestamp;
    std::uint32_t which;
};
static_assert(offsetof(GamepadEventPrefix, which) == 16);

using GetGamepadId = std::uint32_t (*)(SDL_Gamepad*);
GetGamepadId get_gamepad_id = nullptr;
std::uint32_t selected_id = 0;

std::uint32_t current_selected_id() {
    const s32 index = PADGetIndexForPort(PAD_CHAN0);
    if (index < 0) return 0;
    SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
    return gamepad == nullptr ? 0 : get_gamepad_id(gamepad);
}

void reset_held_menu_input() {
    // Dusklight uses focus loss to clear its shared gamepad repeat and axis
    // state. Call the original handler directly, avoiding our own hook.
    SDL_Event event{};
    event.type = window_focus_lost;
    MenuInputEvent::g_orig(event);
}

void sync_selected_device() {
    const std::uint32_t current = current_selected_id();
    if (current == selected_id) return;
    reset_held_menu_input();
    selected_id = current;
}

HookAction filter_menu_gamepad(ModContext*, void* args, void*, void*) {
    const auto& event = mods::arg_ref<SDL_Event&>(args, 0);
    if (event.type != gamepad_axis_motion && event.type != gamepad_button_down &&
        event.type != gamepad_button_up)
    {
        return HOOK_CONTINUE;
    }

    sync_selected_device();
    GamepadEventPrefix prefix{};
    std::memcpy(&prefix, &event, sizeof(prefix));
    return selected_id != 0 && prefix.which == selected_id ? HOOK_CONTINUE :
                                                        HOOK_SKIP_ORIGINAL;
}
}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError* error) {
    void* symbol = nullptr;
    const ModResult lookup = svc_hook->resolve(mod_ctx, "SDL_GetGamepadID", &symbol, nullptr);
    if (lookup != MOD_OK || symbol == nullptr) {
        return mods::set_error(error, lookup == MOD_OK ? MOD_UNAVAILABLE : lookup,
            "Could not resolve SDL_GetGamepadID from Dusklight");
    }
    get_gamepad_id = reinterpret_cast<GetGamepadId>(symbol);
    selected_id = current_selected_id();

    const ModResult result = mods::hook::add_pre<MenuInputEvent>(filter_menu_gamepad);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "Could not hook Dusklight menu input");
    }
    svc_log->info(mod_ctx, "Menu controller input limited to player one's selected device");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    sync_selected_device();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    mods::hook::uninstall<MenuInputEvent>();
    return MOD_OK;
}
}
