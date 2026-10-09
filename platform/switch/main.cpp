// Native Nintendo Switch frontend (libnx + SDL2), derived from platform/vita/main.cpp.
// Shared board/recompiled code; SDL2 presentation/audio; libnx pad input.
#define SDL_MAIN_HANDLED
#include "audio.h"
#include "controls.h"
#include "diagnostic_log.h"
#include "performance.h"
#include "text.h"
#include "workers.h"
#include "default_nvram.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include <switch.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
// M2_ROMSET is "daytona93" or "daytona", fixed by the host recompilation.
#define DAYTONA_DIR "sdmc:/switch/" M2_ROMSET
constexpr const char *kDirectory = DAYTONA_DIR;
constexpr const char *kRom = DAYTONA_DIR "/" M2_ROMSET ".zip";
constexpr const char *kLogPath = DAYTONA_DIR "/switch.log";
// The menu and game are laid out on the Vita's 960x544 canvas; SDL scales
// that logical size to the 1280x720 (handheld) or 1920x1080 (docked) output.
constexpr int kDisplayWidth = 960, kDisplayHeight = 544;
constexpr int kWindowWidth = 1280, kWindowHeight = 720;
PadState g_pad;
// Cabinet switches held in game: left stick click = TEST, right stick click = SERVICE.
bool g_test_held = false, g_service_held = false;
#ifndef DAYTONA_SWITCH_DIAGNOSTICS
#define DAYTONA_SWITCH_DIAGNOSTICS 0
#endif
// Routine logging only in diagnostic builds (build_switch.py --diagnostics).
// Errors are always appended with fault(), so a problem still leaves a record.
vita::DiagnosticLog diagnostics{DAYTONA_SWITCH_DIAGNOSTICS != 0}; // only the main thread writes this log


// Do not use stdio or allocate in this error path: the heap may be missing,
// or freopen may have closed stderr before failing to open its replacement.
void startup_error(const char *message, size_t length) {
    diagnostics.append(message, length);
    if (std::FILE *file = std::fopen(kLogPath, "wb")) {
        std::fwrite(message, 1, length, file);
        std::fclose(file);
    }
}

bool initialize_logging() {
    // A failed startup heap reservation leaves newlib's allocator unusable.
    // Check before freopen/stdio, which can themselves need heap storage.
    void *probe = std::malloc(1024);
    if (!probe) {
        static constexpr char error[] =
            "startup: heap unavailable; stopped before SDL/ROM loading.\n"
            "Launch through a game (title override, hold R) for full memory.\n";
        startup_error(error, sizeof(error) - 1);
        return false;
    }
    std::free(probe);
    if (!diagnostics.enabled()) return true; // quiet build: no stdio log file
    if (!std::freopen(kLogPath, "w", stderr)) {
        static constexpr char error[] =
            "startup: cannot open the stdio log; stopped without using closed stderr.\n"
            "Check " DAYTONA_DIR "/ and free SD space.\n";
        startup_error(error, sizeof(error) - 1);
        return false;
    }
    // No dynamically allocated line buffer is needed for the diagnostic log.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

// System tick in microseconds; works before SDL timer initialization.
uint64_t profile_ticks() { return armTicksToNs(armGetSystemTick()) / 1000; }

uint8_t stick_byte(int value, bool invert) {
    // libnx sticks are -32767..32767 with +Y up; the Vita mapping expects
    // 0..255 with 128 centred and +Y down.
    const int v = 128 + (invert ? -value : value) / 256;
    return uint8_t(std::clamp(v, 0, 255));
}

vita::Pad read_pad() {
    padUpdate(&g_pad);
    const u64 held = padGetButtons(&g_pad);
    const HidAnalogStickState left = padGetStickPos(&g_pad, 0);
    const HidAnalogStickState right = padGetStickPos(&g_pad, 1);
    vita::Pad pad;
    pad.lx = stick_byte(left.x, false);
    pad.ry = stick_byte(right.y, true);
    // Nintendo layout: A confirms (Cross), B goes back (Circle).
    // ZR/ZL are gas/brake; R/L shift up/down like the D-pad; +/- are Start/Coin.
    const struct { u64 native; uint32_t portable; } map[] = {
        {HidNpadButton_A, vita::Cross}, {HidNpadButton_B, vita::Circle},
        {HidNpadButton_Y, vita::Square}, {HidNpadButton_X, vita::Triangle},
        {HidNpadButton_Up, vita::Up}, {HidNpadButton_Down, vita::Down},
        {HidNpadButton_Left, vita::Left}, {HidNpadButton_Right, vita::Right},
        {HidNpadButton_R, vita::Up}, {HidNpadButton_L, vita::Down},
        {HidNpadButton_ZL, vita::L}, {HidNpadButton_ZR, vita::R},
        {HidNpadButton_Plus, vita::Start}, {HidNpadButton_Minus, vita::Select}
    };
    for (auto entry : map) if (held & entry.native) pad.buttons |= entry.portable;
    g_test_held = held & HidNpadButton_StickL;
    g_service_held = held & HidNpadButton_StickR;
    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    // Bound the allocation even if a save has been replaced by a huge file.
    std::vector<uint8_t> temporary(size);
    const bool ok = std::fread(temporary.data(), 1, size, file) == size &&
                    std::fgetc(file) == EOF && !std::ferror(file);
    std::fclose(file);
    if (ok) std::copy(temporary.begin(), temporary.end(), data);
    return ok;
}
bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string temporary = path + ".tmp", backup = path + ".bak";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool ok = std::fwrite(data, 1, size, file) == size;
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) return false;
    // Do not rely on rename replacing an existing destination. Retain one
    // complete previous generation and restore it if the final rename fails.
    bool had_old = false;
    if (auto *old = std::fopen(path.c_str(), "rb")) {
        std::fclose(old);
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0) return false;
        had_old = true;
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0) return true;
    if (had_old) std::rename(backup.c_str(), path.c_str());
    return false;
}
// True when the save (or its backup copy) was on the SD card.
template<class C> bool load_nv(const std::string &name, C &data) {
    const std::string path = std::string(kDirectory) + "/" + name;
    return load_bytes(path, data.data(), data.size()) || load_bytes(path + ".bak", data.data(), data.size());
}
void sdl_check(bool ok, const char *operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

int run_app() {
    diagnostics.literal("stage: run_app begin\n");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
    diagnostics.literal("stage: create window begin\n");
    Window window(SDL_CreateWindow("Daytona Recomp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   kWindowWidth, kWindowHeight, 0), SDL_DestroyWindow);
    sdl_check(bool(window), "create window");
    diagnostics.literal("stage: create window done; create renderer begin\n");
    // No vsync: the game's own 57.5 Hz clock paces the frames. With vsync a
    // frame just over 16.7 ms waits for the next refresh and drops to 30 FPS.
    Renderer renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED), SDL_DestroyRenderer);
    sdl_check(bool(renderer), "create Switch renderer");
    SDL_RendererInfo info{};
    SDL_GetRendererInfo(renderer.get(), &info);
    diagnostics.log("renderer: %s\n", info.name ? info.name : "unknown");
    diagnostics.literal("stage: logical size begin\n");
    sdl_check(SDL_RenderSetLogicalSize(renderer.get(), kDisplayWidth, kDisplayHeight) == 0, "logical size");
    diagnostics.literal("stage: logical size done; create texture begin\n");
    Texture screen(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                      rt::GameLoop::kWidth, rt::GameLoop::kHeight), SDL_DestroyTexture);
    sdl_check(bool(screen), "create screen texture");
    diagnostics.literal("stage: create texture done\n");
    SDL_SetTextureBlendMode(screen.get(), SDL_BLENDMODE_NONE);
    vita::Audio audio;
    diagnostics.literal("stage: audio subsystem begin\n");
    const int audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO);
    diagnostics.log("stage: audio subsystem returned %d\n", audio_init);
    if (audio_init == 0) {
        diagnostics.literal("stage: audio device and resamplers begin\n");
        const bool audio_ok = audio.open();
        diagnostics.log("stage: audio device and resamplers returned %d (%s)\n",
                        int(audio_ok), audio_ok ? "OK" : SDL_GetError());
    } else diagnostics.log("audio unavailable: %s\n", SDL_GetError());
    diagnostics.literal("stage: settings load begin\n");
    uint8_t mute_setting = 0;
    const std::string settings = std::string(kDirectory) + "/mute.bin";
    if (!load_bytes(settings, &mute_setting, 1)) load_bytes(settings + ".bak", &mute_setting, 1);
    bool muted = mute_setting != 0;
    // Performance/input bar at the bottom of the game screen; off by default.
    uint8_t overlay_setting = 0;
    const std::string overlay_file = std::string(kDirectory) + "/overlay.bin";
    if (!load_bytes(overlay_file, &overlay_setting, 1)) load_bytes(overlay_file + ".bak", &overlay_setting, 1);
    bool show_overlay = overlay_setting != 0;
    diagnostics.literal("stage: settings loaded; audio mute begin\n");
    audio.mute(muted);
    diagnostics.literal("stage: audio mute done\n");

    // Cores 1 and 2: raster lanes during the board frame; sound overlaps the next one.
    nx::RasterLanes raster_lanes;
    const int lanes_available = raster_lanes.open();
    nx::SoundThread sound_thread;
    const bool sound_threaded = sound_thread.open();
    diagnostics.log("workers: raster lanes %d, sound thread %d\n", lanes_available, int(sound_threaded));
    bool multicore = true;
    auto apply_cores = [&](rt::GameLoop &g) {
        const int lanes = multicore ? lanes_available : 1;
        g.board().video().set_raster_parallel(
            [&raster_lanes](int count, const std::function<void(int)> &job) { raster_lanes.run(count, job); }, lanes);
    };
    rt::GameLoop::SoundPacket sound_packet; // in flight on sound_thread
    std::unique_ptr<rt::GameLoop> game;
    // Declared after game, so destroyed first: never free the game under a running sound frame.
    struct JoinOnExit { nx::SoundThread &thread; ~JoinOnExit() { thread.wait(); } } join_on_exit{sound_thread};
    // Join the sound frame in flight (before touching the sound board, the
    // audio device or the game). Throws what the sound frame threw.
    auto finish_sound = [&]() -> uint64_t {
        std::string error;
        const uint64_t ticks = sound_thread.wait(&error);
        sound_packet = rt::GameLoop::SoundPacket{};
        if (!error.empty()) throw std::runtime_error(error);
        return ticks;
    };
    auto finish_sound_quietly = [&]() {
        try { finish_sound(); } catch (const std::exception &error) { diagnostics.fault("sound: %s\n", error.what()); }
    };
    std::vector<uint8_t> saved_eeprom, saved_backup;
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1); // one complete game frame per presentation
    bool running = true, menu = true, have_frame = false, wait_release = true;
    bool background = false;
    bool booting = true; // the automatic start at launch
    bool presented_frame_due = false; // a game frame was produced since the last present
    int selection = 0;
    uint8_t pulse = 0;
    uint32_t previous_buttons = 0;
    constexpr double frequency = 1000000.0; // profile_ticks() is native microseconds
    vita::Performance performance(frequency);
    diagnostics.log("performance diagnostics enabled; max game steps per present: 1; clock settings unchanged\n");
#ifdef M2_VITA_RENDER_OPT
    diagnostics.literal("render_opt: OPT03 enabled - exact tile/layer cache, mip setup, lighting LUT and packed texture reads\n");
#else
    diagnostics.literal("render_opt: OPT03 reference renderer - optimization disabled\n");
#endif
#ifdef NDEBUG
    diagnostics.log("build: NDEBUG defined (assertions disabled)\n");
#else
    diagnostics.log("build: assertions enabled; check Release optimization flags\n");
#endif
    uint64_t previous_counter = profile_ticks();
    double save_elapsed = 0;
    std::string status = "Place " M2_ROMSET ".zip in " DAYTONA_DIR "/ then select START GAME.";
    {
        const AppletType type = appletGetAppletType();
        diagnostics.log("applet type: %d\n", int(type));
        if (type != AppletType_Application && type != AppletType_SystemApplication)
            status = "APPLET MODE: memory is limited. Launch from a game holding R if START fails. " + status;
    }
    char performance_screen[160] = "WAITING FOR COMPLETED FRAMES";
    unsigned traced_frames = 0;
    unsigned startup_presents = 0;
    // Optional performance bar (menu: PERFORMANCE BAR): frame rate and timings only.
    constexpr int kBarHeight = 18;
    auto draw_performance_bar = [&](int y) {
        SDL_SetRenderDrawColor(renderer.get(), 16, 18, 24, 255);
        SDL_Rect background_rect{0, y, kDisplayWidth, kBarHeight};
        SDL_RenderFillRect(renderer.get(), &background_rect);
        SDL_SetRenderDrawColor(renderer.get(), 235, 235, 235, 255);
        vita::text(renderer.get(), performance_screen, 18, y + 2, 2, 76, 1);
    };

    auto save = [&]() {
        if (!game) return true;
        auto save_changed = [&](const char *name, const auto &data, std::vector<uint8_t> &previous) {
            if (data.size() == previous.size() && std::equal(data.begin(), data.end(), previous.begin())) return true;
            if (!save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size())) return false;
            previous.assign(data.begin(), data.end());
            return true;
        };
        // Do not short-circuit: attempt both saves even if one failed.
        const bool eeprom_ok = save_changed("ioboard_eeprom.bin", game->board().io().eeprom, saved_eeprom);
        const bool backup_ok = save_changed("backup_ram.bin", game->board().backup_ram(), saved_backup);
        if (!eeprom_ok || !backup_ok) {
            status = "Save failed. Check free space and " DAYTONA_DIR "/. Previous saves are kept as .bak.";
            diagnostics.fault("%s\n", status.c_str());
        }
        return eeprom_ok && backup_ok;
    };
    constexpr int kMenuItems = 8;
    auto draw_menu = [&]() {
        SDL_SetRenderDrawColor(renderer.get(), 16, 18, 24, 255);
        SDL_RenderClear(renderer.get());
        SDL_SetRenderDrawColor(renderer.get(), 235, 235, 235, 255);
        vita::text(renderer.get(), "DAYTONA RECOMP - SWITCH", 30, 28, 3, 49, 1);
        const std::string labels[] = {game ? "RESUME GAME" : "START GAME", "RESET GAME", "TEST SWITCH", "SERVICE COIN",
                                      muted ? "SOUND: MUTED" : "SOUND: ON",
                                      multicore ? "CPU CORES: 3 (FAST)" : "CPU CORES: 1 (COMPARE)",
                                      show_overlay ? "PERFORMANCE BAR: ON" : "PERFORMANCE BAR: OFF", "SAVE AND QUIT"};
        for (int i = 0; i < kMenuItems; ++i) {
            if (i == selection) SDL_SetRenderDrawColor(renderer.get(), 255, 200, 70, 255);
            else SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
            vita::text(renderer.get(), (i == selection ? "> " : "  ") + labels[i], 42, 104 + i * 34, 2, 70, 1);
        }
        SDL_SetRenderDrawColor(renderer.get(), 220, 220, 225, 255);
        vita::text(renderer.get(), status, 30, 384, 2, 74, 6);
        vita::text(renderer.get(), "A: SELECT  B: RESUME  PLUS+MINUS: MENU", 30, 516, 2, 74, 1);
    };
    auto reset_clock = [&]() {
        clock.reset(); previous_counter = profile_ticks(); wait_release = true;
        performance.reset(previous_counter, menu);
        std::snprintf(performance_screen, sizeof performance_screen, "WAITING FOR COMPLETED FRAMES");
    };
    auto open_menu = [&]() {
        diagnostics.literal("stage: pause/menu begin\n");
        finish_sound_quietly();
        menu = true; audio.pause(); save(); reset_clock();
        diagnostics.literal("stage: pause/menu done\n");
    };
    auto start = [&]() {
        diagnostics.literal("stage: start/reset begin\n");
        traced_frames = 0;
        finish_sound_quietly();
        if (!save()) return false;
        game.reset(); have_frame = false; audio.pause();
        status = "Loading and checking your ROM set...";
        if (booting) {
            // Straight into the game at launch: a black screen, not the menu.
            SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
            SDL_RenderClear(renderer.get());
        } else draw_menu();
        diagnostics.literal("stage: loading screen present begin\n");
        SDL_RenderPresent(renderer.get());
        diagnostics.literal("stage: loading screen present done\n");
        // Check the file without throwing: some emulators (Ryujinx) cannot run
        // the C++ unwinder of current devkitA64, so a thrown error closes them.
        if (std::FILE *rom = std::fopen(kRom, "rb")) std::fclose(rom);
        else {
            status = std::string("ROM NOT FOUND: ") + kRom;
            diagnostics.fault("start: %s\n", status.c_str());
            reset_clock();
            return false;
        }
        try {
            diagnostics.literal("stage: ROM import begin\n");
            auto images = rt::import_rom_set(kRom);
            diagnostics.literal("stage: ROM import done; board creation begin\n");
            game = std::make_unique<rt::GameLoop>(std::move(images));
            diagnostics.literal("stage: board creation done; save loading begin\n");
            game->set_profile_clock(profile_ticks);
            game->board().video().set_profile_clock(profile_ticks);
            apply_cores(*game);
            auto &eeprom = game->board().io().eeprom;
            auto &backup = game->board().backup_ram();
            const bool have_eeprom = load_nv("ioboard_eeprom.bin", eeprom);
            load_nv("backup_ram.bin", backup);
            // First run of Revision A: start as a single cabinet, not a linked
            // twin waiting for a second one. Saves on the SD card always win.
            if (std::string_view(M2_ROMSET) == "daytona" && !have_eeprom) {
                // Settings only: the game builds its backup RAM from them (tested).
                std::copy(std::begin(nx::kDaytonaDefaultEeprom), std::end(nx::kDaytonaDefaultEeprom), eeprom.begin());
                diagnostics.log("first run: single-cabinet factory settings\n");
            }
            saved_eeprom.assign(game->board().io().eeprom.begin(), game->board().io().eeprom.end());
            saved_backup = game->board().backup_ram();
            diagnostics.literal("stage: save loading done\n");
            controls = vita::Controls{};
            pulse = 0; save_elapsed = 0;
            status = "ZL/ZR: BRAKE/GAS. LEFT STICK: STEER. RIGHT STICK: ANALOG PEDALS. L/R OR UP/DOWN: GEARS. FACE BUTTONS: VIEWS. MINUS: COIN. PLUS: START. L3: TEST. R3: SERVICE.";
            reset_clock();
            return true;
        } catch (const std::exception &error) {
            status = error.what(); diagnostics.fault("start: %s\n", error.what());
            reset_clock();
            return false;
        }
    };

    // Start the game at launch. If it cannot start (no ROM set), the menu
    // stays open and says why. The menu is always available with + and -.
    if (start()) { menu = false; reset_clock(); }
    booting = false;

    diagnostics.literal("stage: main loop entered\n");
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_APP_WILLENTERBACKGROUND) { background = true; open_menu(); }
            else if (event.type == SDL_APP_DIDENTERFOREGROUND) { background = false; reset_clock(); }
        }
        if (!running) break;
        if (background) { SDL_Delay(20); continue; }
        const auto pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0;
            controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) open_menu();
        if (menu && !wait_release) {
            if (pressed & vita::Up) selection = (selection + kMenuItems - 1) % kMenuItems;
            if (pressed & vita::Down) selection = (selection + 1) % kMenuItems;
            if ((pressed & vita::Circle) && game) { menu = false; reset_clock(); }
            else if (pressed & vita::Cross) {
                switch (selection) {
                case 0: if (game || start()) { menu = false; reset_clock(); } break;
                case 1: if (start()) { menu = false; reset_clock(); } break;
                case 2: case 3:
                    if (game) { pulse = selection == 2 ? 0x04 : 0x08; menu = false; reset_clock(); }
                    else status = "Start the game before using the cabinet switches.";
                    break;
                case 4:
                    muted = !muted; audio.mute(muted); mute_setting = muted ? 1 : 0;
                    if (!save_bytes(settings, &mute_setting, 1)) status = "Unable to save sound setting.";
                    break;
                case 5:
                    multicore = !multicore;
                    if (game) apply_cores(*game);
                    break;
                case 6:
                    show_overlay = !show_overlay; overlay_setting = show_overlay ? 1 : 0;
                    if (!save_bytes(overlay_file, &overlay_setting, 1)) status = "Unable to save the performance bar setting.";
                    break;
                case 7: if (save()) running = false; break;
                }
            }
        }
        const uint64_t now = profile_ticks();
        const double elapsed = double(now - previous_counter) / frequency;
        previous_counter = now;
        performance.begin(now, menu);
        bool trace_frame = false;
        if (game && !menu) {
            const int frames = clock.advance(elapsed);
            if (frames) presented_frame_due = true;
            trace_frame = frames > 0 && traced_frames < 2;
            try {
                for (int n = 0; n < frames; ++n) {
                    const auto input = controls.sample(wait_release ? vita::Pad{} : pad);
                    rt::Inputs mapped;
                    mapped.steer = input.steer; mapped.accel = input.accel; mapped.brake = input.brake;
                    uint8_t switches = pulse;
                    if (g_test_held && !wait_release) switches |= 0x04;
                    if (g_service_held && !wait_release) switches |= 0x08;
                    mapped.in0 = uint8_t(input.in0 & ~switches); mapped.in1 = input.in1; mapped.in2 = input.in2;
                    if (trace_frame) diagnostics.log("stage: frame %u run_frame begin\n", traced_frames);
                    // Main board frame N+1 while the sound worker finishes frame N.
                    rt::GameLoop::SoundPacket next = game->run_frame_sound_packet(mapped); pulse = 0;
                    if (trace_frame) diagnostics.log("stage: frame %u run_frame done\n", traced_frames);
                    const uint64_t sound_ticks = finish_sound(); // the previous sound frame: its samples are ready
                    rt::FrameProfile profile = game->last_profile();
                    profile.sound = sound_ticks; // worker time, overlapped with the main board
                    profile.total += sound_ticks;
                    performance.frame(profile);
                    performance.video(game->board().video().last_profile());
                    if (game->sound()) {
                        const uint64_t before_audio = profile_ticks();
                        if (trace_frame) diagnostics.literal("stage: audio push begin\n");
                        audio.push(*game->sound());
                        if (trace_frame) diagnostics.literal("stage: audio push done\n");
                        performance.span(vita::Performance::Audio, before_audio, profile_ticks());
                    }
                    sound_packet = std::move(next);
                    sound_thread.submit([&sound_packet] { return sound_packet.execute(); });
                    have_frame = true;
                    ++traced_frames;
                }
                if (frames) {
                    if (trace_frame) diagnostics.literal("stage: texture upload begin\n");
                    const uint64_t before_upload = profile_ticks();
                    sdl_check(SDL_UpdateTexture(screen.get(), nullptr, game->screen().data(),
                              rt::GameLoop::kWidth * int(sizeof(uint32_t))) == 0, "upload screen");
                    performance.span(vita::Performance::Upload, before_upload, profile_ticks());
                    if (trace_frame) diagnostics.literal("stage: texture upload done\n");
                }
                save_elapsed += elapsed;
                if (save_elapsed >= 5.0) {
                    const uint64_t before_save = profile_ticks();
                    save(); save_elapsed = 0;
                    performance.span(vita::Performance::Save, before_save, profile_ticks());
                }
            } catch (const std::exception &error) {
                status = error.what(); diagnostics.fault("runtime: %s\n", error.what());
                open_menu(); finish_sound_quietly(); game.reset(); have_frame = false;
            }
        } else clock.reset();
        if (game && !menu && have_frame && !presented_frame_due) {
            // No new frame: do not redraw the same one (vsync is off).
            SDL_Delay(1);
            continue;
        }
        presented_frame_due = false;
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: draw begin\n");
        const uint64_t before_draw = profile_ticks();
        if (menu) draw_menu();
        else {
            SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
            SDL_RenderClear(renderer.get());
            if (have_frame) {
                // Preserve the same square-pixel framebuffer aspect as desktop.
                const int width = kDisplayHeight * rt::GameLoop::kWidth / rt::GameLoop::kHeight;
                const SDL_Rect destination{(kDisplayWidth - width) / 2, 0, width, kDisplayHeight};
                SDL_RenderCopy(renderer.get(), screen.get(), nullptr, &destination);
            }
        }
        if (!menu && show_overlay) draw_performance_bar(kDisplayHeight - kBarHeight);
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: draw done; present begin\n");
        const uint64_t before_present = profile_ticks();
        performance.span(vita::Performance::Draw, before_draw, before_present);
        SDL_RenderPresent(renderer.get());
        const uint64_t after_present = profile_ticks();
        if (startup_presents < 2 || trace_frame) diagnostics.literal("stage: present done\n");
        if (startup_presents < 2) ++startup_presents;
        performance.span(vita::Performance::Present, before_present, after_present);
        performance.presented();
        if (performance.ready(after_present)) {
            char line[768];
            const int count = performance.format(line, sizeof line, after_present, 0, 0, 0);
            // One bounded write every two seconds; not a frame-by-frame trace.
            if (count > 0) diagnostics.append(line, std::min(size_t(count), sizeof(line) - 1));
            performance.format_overlay(performance_screen, sizeof performance_screen, after_present);
            performance.reset(after_present, menu);
        }
        SDL_Delay(menu ? 16 : 1); // the menu needs no more than ~60 FPS
    }
    diagnostics.literal("stage: save/quit begin\n");
    finish_sound_quietly();
    save();
    diagnostics.literal("stage: save/quit done\n");
    return 0;
}
} // namespace

int main(int, char **) {
    mkdir("sdmc:/switch", 0777);
    mkdir(kDirectory, 0777);
    diagnostics.begin(); // log begins BEFORE heap, stdio, SDL, texture or audio setup
    diagnostics.literal("stage: heap and stdio probe begin\n");
    if (!initialize_logging()) {
        return 1;
    }
    diagnostics.log("build: switch %s %s; native diagnostic path: %s\n", __DATE__, __TIME__, vita::DiagnosticLog::kPath);
    diagnostics.literal("stage: heap and stdio probe done; SDL initialization begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        diagnostics.fault("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    diagnostics.literal("stage: SDL initialization done\n");
    int result = 0;
    try { result = run_app(); }
    catch (const std::exception &error) { diagnostics.fault("fatal: %s\n", error.what()); result = 1; }
    diagnostics.literal("stage: SDL quit begin\n");
    SDL_Quit();
    diagnostics.log("stage: clean exit %d\n", result);
    return result;
}
