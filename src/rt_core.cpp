// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the ReTraced project
//
// The core's entries (rt_api.h). Chantier 0: everything is inert. The core watches (devices, modules, frames), measures
// its own cost and writes rt_report.json; it changes nothing in the picture.
#define RT_BUILDING_CORE
#include "rt_api.h"
#include "rt_diag.h"
#include "rt_display.h"
#include "rt_spv.h"
#include "rt_gbuf.h"
#include "rt_veg.h"
#include "rt_vr.h"
#include "rt_xr.h"
#include "rt_world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define RT_STR_(x) #x
#define RT_STR(x) RT_STR_(x)
#ifdef RT_CORE_BUILD_ID
#define RT_CORE_BUILD RT_STR(RT_CORE_BUILD_ID)
#else
#define RT_CORE_BUILD "dev"
#endif

namespace rt {

void (*host_log_fn)(const char*);

namespace {

struct State {
    bool active = false;
    std::string host_name, host_revision;
    int64_t t_init = 0, t_device = 0, t_first_frame = 0;
    std::string gpu_name;
    std::string displays = "[]";
    uint32_t driver_version = 0, api_version = 0;

    // shader modules (any thread)
    std::atomic<uint64_t> modules{0}, modules_patched{0}, words{0}, spirv_ns{0};
    std::mutex unique_mu;
    std::unordered_set<uint64_t> unique;

    // frames (host's replay lock)
    uint64_t frames = 0;
    int64_t frame_ns_total = 0, frame_ns_max = 0, report_ns_max = 0;
    int64_t report_every = 300;
    uint64_t next_report = 60;
    int64_t t_last_report = 0;
    uint64_t frames_at_last_report = 0;
    double fps_window = 0;
} S;

const RtHost* g_host = nullptr;

// ---- SPIR-V patches ----
// Our patched modules live in <dll dir>\retraced\patches\<fnv64 of the original words>.<tag>.spv, made on this PC by
// tools\patch-*.py from the player's own game; never in git. Loaded once at init; read-only afterwards.
struct Patch {
    std::vector<uint32_t> words;
    bool curve = false;                   // (tag "courbe": our tone curve in the game's HDR curve program)
    bool gbuf = false;                    // (tag "gbuf": a lit program with our per-pixel outputs)
};
std::unordered_map<uint64_t, Patch> g_patches;
std::unordered_map<uint64_t, std::string> g_gbuf_files;   // (tag "gbuf": hash of the original module -> file, read on demand)
std::atomic<uint64_t> g_gbuf_read{0}, g_gbuf_bytes{0};
std::unordered_set<uint64_t> g_gbuf_sun;                   // (those of them that carry the sun switch: gbuf-sun.txt)
std::mutex g_xfb_mu;                      // (modules edited on the fly for the transform feedback capture, until released)
std::unordered_map<const uint32_t*, std::vector<uint32_t>*> g_xfb_live;
std::atomic<uint64_t> g_xfb_patched{0};
thread_local uint32_t g_veg_slots = 0;
std::atomic<bool> g_curve_patched{false};
std::vector<uint32_t> g_spv_sig;          // (spirv_dump_sig = hex words: modules holding all of them are dumped)
std::mutex g_dump_mu;
std::unordered_set<uint64_t> g_dumped;

void load_patches() {
    const std::string dir = dll_dir() + "\\retraced\\patches";
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*.spv").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name.size() < 20) continue;
        const uint64_t h = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
        FILE* in = std::fopen((dir + "\\" + name).c_str(), "rb");
        if (!in) continue;
        Patch p;
        p.words.resize(fd.nFileSizeLow / 4);
        const size_t got = std::fread(p.words.data(), 4, p.words.size(), in);
        std::fclose(in);
        if (got != p.words.size() || p.words.empty() || p.words[0] != 0x07230203u) {
            log_host("spirv: patch %s unreadable or not SPIR-V, left out", name.c_str());
            continue;
        }
        // (tag "gbuf": a lit program with our added outputs; only with the option gbuf=1)
        if (name.find(".gbuf.") != std::string::npos) {
            if (opt_int("gbuf", 0) != 0) g_gbuf_files[std::strtoull(name.substr(0, 16).c_str(), nullptr, 16)] = dir + "\\" + name;
            continue;
        }
        p.curve = name.find(".courbe.") != std::string::npos;
        p.gbuf = name.find(".gbuf.") != std::string::npos;
        if (p.curve && opt_int("hdr_curve_patch", 0) == 0) {
            log_host("spirv: patch %s not used (hdr_curve_patch=0: the game's own expansion formula is used)", name.c_str());
            continue;
        }
        static int told = 0;   // (the first few only: a table of lit programs holds thousands)
        if (told++ < 8)
            log_host("spirv: patch %s loaded (%zu words) for module %016llx", name.c_str(), p.words.size(), (unsigned long long)h);
        g_patches[h] = std::move(p);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    if (!g_gbuf_files.empty()) {
        if (FILE* in = std::fopen((dir + "\\gbuf-sun.txt").c_str(), "r")) {
            char line[64];
            while (std::fgets(line, sizeof line, in))
                if (const uint64_t h = std::strtoull(line, nullptr, 16)) g_gbuf_sun.insert(h);
            std::fclose(in);
        }
        log_host("spirv: %zu lit programs with our per-pixel outputs listed (read when their module is made), %zu with the sun switch",
                 g_gbuf_files.size(), g_gbuf_sun.size());
    }
}

void spirv_dump_if_match(uint64_t h, const uint32_t* w, size_t n) {
    for (uint32_t s : g_spv_sig)
        if (std::find(w, w + n, s) == w + n) return;
    std::lock_guard lk(g_dump_mu);
    if (!g_dumped.insert(h).second) return;
    const std::string dir = out_dir() + "\\spv";
    CreateDirectoryA(dir.c_str(), nullptr);
    char name[64];
    std::snprintf(name, sizeof name, "\\%016llx.spv", (unsigned long long)h);
    if (FILE* o = std::fopen((dir + name).c_str(), "wb")) {
        std::fwrite(w, 4, n, o);
        std::fclose(o);
        log_host("spirv: module %016llx dumped (signature matched, %zu words)", (unsigned long long)h, n);
    }
}

bool g_hdr_metadata = false;              // (VK_EXT_hdr_metadata enabled on the device)
VkDevice g_device = VK_NULL_HANDLE;
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;
DisplayInfo g_primary;                    // (the display the game is told about)

// FNV-1a 64 over the module's 32-bit words: the key of the patch table.
uint64_t fnv1a64(const uint32_t* w, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char* p = (const unsigned char*)w;
    for (size_t i = 0, e = n * 4; i < e; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- watched uniform blocks (the game's tone curve values) ----
struct Seen {
    uint32_t source = 2, size = 0;
    uint64_t count = 0;
    uint8_t bytes[2048] = {};
};
std::mutex uni_mu;
std::vector<uint64_t> watched;                  // from the option watch_uniforms (hex keys, comma separated)
std::map<std::tuple<uint64_t, int, uint32_t>, Seen> seen;

std::string uniforms_json() {
    std::lock_guard lk(uni_mu);
    std::string s = "[";
    bool first = true;
    for (auto& [k, v] : seen) {
        char h[160];
        std::snprintf(h, sizeof h, "%s\n    {\"program\": \"%016llx\", \"stage\": %d, \"bank\": %u, \"source\": %u, \"bytes\": %u, \"count\": %llu, \"floats\": [",
                      first ? "" : ",", (unsigned long long)std::get<0>(k), std::get<1>(k), std::get<2>(k), v.source, v.size,
                      (unsigned long long)v.count);
        s += h;
        const uint32_t nf = std::min<uint32_t>(v.size, sizeof v.bytes) / 4;
        for (uint32_t i = 0; i < nf; ++i) {
            float f;
            std::memcpy(&f, v.bytes + i * 4, 4);
            char t[32];
            std::snprintf(t, sizeof t, "%s%.7g", i ? ", " : "", std::isfinite(f) ? f : 0.0f);
            s += t;
        }
        s += "]}";
        first = false;
    }
    return s + (first ? "]" : "\n  ]");
}

uint32_t rt_watched_programs(uint64_t* keys, uint32_t cap) {
    uint32_t n = 0;
    for (uint64_t k : watched)
        if (n < cap) keys[n++] = k;
    return n;
}

// hdr_full_peak = 1: the game's HDR tone curve (babc4ebfa024ffac, fragment c3) gets the display's own peak / paper white
// in c3[0x40], in place of its cap of 3.5 (measured: c3[0x40] = min(peak, 3.5 x paper) / paper)
constexpr uint64_t kGameHdrCurve = 0xbabc4ebfa024ffacull;
bool g_full_peak = false;
bool g_hdr_curve_watch = false;           // (hdr=1: the game's HDR curve is watched, for the toggle and the peak)
std::atomic<uint64_t> g_substituted{0};

// The effects switch (key ², or RETRACED_EFFECTS=0 at start): off shows the game as it would be without ReTraced. For HDR,
// "off" is the game's own SDR picture at the display's paper white: with c3[0x40] = 1 the HDR curve's expansion is the
// identity (m = 1 gives L' = L) and its clamp is 1.0, so the PQ output is the SDR image as Windows shows SDR content.
std::atomic<bool> g_effects_on{true};
// hdr_strength (key Y cycles it): how far the highlights are expanded. The game's HDR curve expands the luminance L of its
// SDR picture above a knee K up to P times paper white: L' = K + (1-K) m s / ((m-1)(1-s)^g + 1), s = (L-K)/(1-K),
// m = (P-K)/(1-K). Nintendo ships K = 0.65, g = 0.75, P = 3.5: only the last
// few percent below white move, and nothing shows (99.9 % of a daylight frame is below L = 0.75). The same formula
// with a lower knee and a higher g opens the upper tones: the slope at the knee stays 1 for any g (no step).
//   0 = Nintendo's values;  1 soft (K 0.45, g 1.5);  2 medium (K 0.30, g 2.5);  3 strong (K 0.20, g 3.0)
// (measured at the village: with K 0.35 and g 2.0, 99 % of the frame still stayed under 170 nits: a daylight frame of this
// game sits low, the sky itself at about 0.6 of white; the knee has to come down for anything to show)
std::atomic<int> g_hdr_strength{1};   // (1: the default)
std::atomic<uint64_t> g_toggles{0};

// ---- light terms cycle (the weight of each lighting term) ----
// light_terms = 1: every program's uniform blocks are seen; in the lit programs' fragment banks, env = c6 (gsys_environment,
// 1680 bytes) and scn = c7 (gsys_scene_material), one term is zeroed per state, each state lasting light_terms_every frames:
// 0 none, 1 sun (env 0x50-0x58), 2 hemisphere (env 0x490-0x4a8), 3 baked (env 0x440, 0x460-0x468), 4 local lights
// (env 0x710), 5 cube ambient diffuse (scn 0x20), 6 cube ambient specular (scn 0x24), 7 all (only emission remains).
bool g_terms = false;
// rt_sun_fade (on with the sun's rays, option 0 to leave the game's values): the terrain and lightmapped programs fade
// the mask's sun shadow out with the view distance (env 0x3e0..0x3e4) and fade a BAKED sun shadow in (env 0x3e8..0x3ec;
// facts: anatomie-eclairage.md, "shadow = sat((1 - mask.g)(1 - t0) + t1 (1 - baked.g))"). In the castle village the
// baked one is there from the first metre: forcing the mask to "no shadow" left every ground shadow in place. With the
// four distances pushed out of reach, the mask alone gives the sun's shadow, at every distance: ours.
bool g_sun_fade = false;
int64_t g_sun_fade_mode = 1;   // (bit 0: the mask kept at every distance; bit 1: the baked shadow never used.
                               // Default 1: our shadows reach every distance and the baked one stays, so that none is lost)
std::atomic<uint64_t> g_fade_subs{0};
// rt_sun_proj: the lit programs also multiply the sun by "projected shadows", two textures laid over the world by two
// projectors of the camera block: proj = sat(ctx[0x290] fade(d) (1 - #210.r) + ctx[0x294] (1 - #211.r)) (facts:
// anatomie-eclairage.md). In the castle village they hold the large shadows of the buildings on the ground: with the
// mask at "no shadow" in both channels those stayed. Bit 0: the first strength (0x290) set to 0; bit 1: the second
// (0x294). Default 0: clouds and leaves may be in them, to be looked at place by place.
int64_t g_sun_proj = 0;
std::atomic<uint64_t> g_proj_subs{0};
// light_oven = K (thousandths): the "oven". Every light of the lit
// programs is cut except the hemisphere floor, whose two colours are set: up = K/1000, down = light_oven_down/1000
// (default: the same). The hemisphere has no occlusion and no scale: the game's lit image is then exactly
// (diffuse colour) x K (+ emission), an oracle for our colour image; with down = 0 it is
// (diffuse colour) x K x (0.5 + 0.5 x the facing's vertical part), an oracle for our facing image.
float g_oven_up = 0, g_oven_down = 0;
bool g_oven = false;
std::atomic<uint64_t> g_oven_subs{0};
// rt_sun_own (on with the sun's rays and the per-pixel programs; 0 to leave the game's term): our instrumented lit
// programs carry a switch on the sun's visibility (tools/gbuf_sun.py): with scn[0x8] below -0.5 the sun is limited by
// the mask's G channel alone (our rays, and the game's cascades with them in the default mode) and by the projected
// shadows, no longer by the screen-space occlusion and the material's occlusion texture. Set only for the draws
// whose fragment module is ours: any other program would read -1 as "no sun at all".
bool g_sun_own = false;
std::atomic<uint64_t> g_own_subs{0};
// rt_gi_cut (the light by rays): in our lit programs (the draws whose fragment module is
// ours, RtUniform.ours bit 0) the game's ambient diffuse light is cut while the menu's "light by rays" is on and the
// effects are on: our light (rt_gi_*.comp) stands in its place. Bits: 1 the baked light's tint (env 0x460-0x468: the
// probes' light and their specular highlight), 2 the hemisphere (env 0x490-0x4a8), 4 the cube's diffuse term
// (scn 0x20), 8 the baked share itself (env 0x440: also the lightmaps and the terrain's baked light, but it also
// takes away the darkening of the cube's reflection in dark places). Facts: anatomie-eclairage.md section 4.6.
// Default 14 (measured in the forest: without bit 8 the cut took 5 % of the picture away, the lightmaps and
// the terrain's baked light stayed, and ours came on top of them; with bit 8, bit 1 is not needed).
// The menu's "share" (0 to 100 %, RtExports live value 6) says how much of the game's ambient light ours takes over:
// the terms are scaled by (1 - share) rather than zeroed, and ours is added times the share. The baked share env 0x440
// scales the probes, the lightmaps, the terrain's baked light and their highlight together; it also weighs the cube's
// diffuse term by (1 - env 0x440), so that term is scaled to keep (1 - share) of its own weight.
bool g_gi_cut = false;
int64_t g_gi_cut_bits = 7;
std::atomic<uint64_t> g_gicut_subs{0};
std::atomic<float> g_e440{1.0f};   // (env 0x440 as the fog program last read it: the same for the whole frame)
int64_t g_terms_every = 240;
constexpr int kTermStates = 8;
const char* const kTermNames[kTermStates] = {"none", "sun", "hemisphere", "baked", "local_lights", "cube_diffuse",
                                             "cube_specular", "all"};
std::atomic<uint64_t> g_term_subs[kTermStates];
std::mutex g_scn_mu;
std::map<uint32_t, uint64_t> g_scn_sizes;   // (sizes of fragment c7 blocks, for the report)
struct C6 { uint64_t count = 0, refused = 0; uint32_t min_size = 0, max_size = 0, sources = 0; };
std::map<uint64_t, C6> g_c6;                 // (fragment c6 blocks by guest address)

int term_state() { return g_terms ? (int)((S.frames / (uint64_t)g_terms_every) % kTermStates) : 0; }

// gsys_environment is one buffer per frame (a ring of a few): its addresses are learnt from the fog program, which binds
// it whole (bank 4, 1680 bytes); programs bind only the part they read, so the address is the only sure mark.
constexpr uint64_t kFogApply = 0x1a7482c0ec9991eaull;
std::mutex g_env_mu;
uint64_t g_env_addr[8] = {};
std::atomic<uint32_t> g_env_n{0};

bool is_env_addr(uint64_t a) {
    if (!a) return false;
    const uint32_t n = std::min<uint32_t>(g_env_n.load(std::memory_order_acquire), 8);
    for (uint32_t i = 0; i < n; ++i)
        if (g_env_addr[i] == a) return true;
    return false;
}

// The game keeps more than one gsys_environment (another pair of buffers, read by about half of the lit draws, never
// by the fog program): a c6 block is also taken for one when its sun colour (0x50-0x5c) and sun direction (0x170-0x17c)
// equal those of the last block the fog program read.
std::mutex g_envsig_mu;
uint8_t g_env_sig[24] = {};
bool g_env_sig_ok = false;

bool is_env_content(const RtUniform* u) {
    if (!u->bytes || u->size < 0x180) return false;
    std::lock_guard lk(g_envsig_mu);
    if (!g_env_sig_ok) return false;
    const uint8_t* b = (const uint8_t*)u->bytes;
    return std::memcmp(b + 0x50, g_env_sig, 12) == 0 && std::memcmp(b + 0x170, g_env_sig + 12, 12) == 0;
}

void env_learn(const RtUniform* u) {
    if (u->program_key != kFogApply || u->stage != 1 || u->bank != 4 || u->size != 1680) return;
    if (u->bytes) {
        float e;
        std::memcpy(&e, (const uint8_t*)u->bytes + 0x440, 4);
        if (std::isfinite(e)) g_e440.store(e, std::memory_order_relaxed);
        std::lock_guard lk(g_envsig_mu);
        std::memcpy(g_env_sig, (const uint8_t*)u->bytes + 0x50, 12);
        std::memcpy(g_env_sig + 12, (const uint8_t*)u->bytes + 0x170, 12);
        g_env_sig_ok = true;
    }
    if (u->guest_addr && !is_env_addr(u->guest_addr)) {
        std::lock_guard lk(g_env_mu);
        static uint32_t cursor = 0;
        g_env_addr[cursor++ % 8] = u->guest_addr;
        g_env_n.store(std::min<uint32_t>(cursor, 8), std::memory_order_release);
        log("light terms: gsys_environment at %#llx", (unsigned long long)u->guest_addr);
    }
}

uint32_t terms_substitute(const RtUniform* u, void* out, uint32_t cap) {
    if (u->program_key == kFogApply) return 0;
    // (the light by rays is made ready with the world and turned on by the menu: while it is off, nothing to do here)
    if (!g_terms && !g_sun_fade && !g_sun_proj && !g_oven && !g_sun_own &&
        !(g_gi_cut && (u->ours & 1) && world::live_get(5) != 0 && g_effects_on.load(std::memory_order_relaxed) &&
          !world::is_water_program(u->program_key)))
        return 0;
    if (g_terms && u->stage == 1 && u->bank == 6) {   // (diagnostic: where the lit programs' c6 points, and how much of it they bind)
        std::lock_guard lk(g_scn_mu);
        auto& e = g_c6[u->guest_addr];
        e.count++;
        e.min_size = e.count == 1 ? u->size : std::min(e.min_size, u->size);
        e.max_size = std::max(e.max_size, u->size);
        e.sources |= 1u << std::min<uint32_t>(u->source, 3);
        if (!u->bytes || u->size == 0 || u->size > cap) e.refused++;
    }
    if (u->stage != 1 || !u->bytes || u->size > cap || u->program_key == kFogApply) return 0;
    const bool env = u->bank == 6 && (is_env_addr(u->guest_addr) || is_env_content(u));
    const bool scn = u->bank == 7 && u->size >= 0x28;
    if (scn && g_terms) {
        std::lock_guard lk(g_scn_mu);
        g_scn_sizes[u->size]++;
    }
    const int st = term_state();
    const bool fade = env && g_sun_fade && u->size >= 0x3f0 && g_effects_on.load(std::memory_order_relaxed);
    const bool proj = g_sun_proj && u->bank == 3 && u->size >= 0x298 && world::is_ctx_addr(u->guest_addr) &&
                      g_effects_on.load(std::memory_order_relaxed);
    const bool oven = g_oven && (env || scn);
    const bool own = g_sun_own && scn && (u->ours & 2) && g_effects_on.load(std::memory_order_relaxed);
    // A blinding white on the water and on everything
    // around it, with the light by rays on and water on screen, which no trial of ours had ever had together. Found
    // value by value (rt_gi_cut = 2, 4, 8): it is env[0x440] taken to zero (see where rt_gi_cut is read).
    //   - the water's own programs ("ours" since the water became a mirror): no cut at all, they are not lit by us;
    //   - what looks like water: never env[0x440], whatever rt_gi_cut says (rt_gi_cut_skip = 2: no cut at all).
    static const int64_t skip_mode = opt_int("rt_gi_cut_skip", 1);
    const int wclass = world::cut_class(u->program_key);
    const int64_t cut_bits = wclass == 2 ? (skip_mode == 2 ? 0 : (g_gi_cut_bits & ~(int64_t)8)) : g_gi_cut_bits;
    const bool gicut = g_gi_cut && (env || scn) && (u->ours & 1) && g_effects_on.load(std::memory_order_relaxed) &&
                       world::live_get(5) != 0 && wclass != 1 && cut_bits != 0;
    if (!(st && (env || scn)) && !fade && !proj && !oven && !own && !gicut) return 0;
    auto zero = [&](uint32_t a, uint32_t b) {   // (words a..b inclusive)
        for (uint32_t o = a; o <= b && o + 4 <= u->size; o += 4) std::memset((uint8_t*)out + o, 0, 4);
    };
    std::memcpy(out, u->bytes, u->size);
    bool changed = false;
    if (oven) {
        auto put = [&](uint32_t o, float v) {
            if (o + 4 <= u->size) std::memcpy((uint8_t*)out + o, &v, 4);
        };
        if (env) {
            zero(0x50, 0x58), zero(0x440, 0x440), zero(0x460, 0x468), zero(0x710, 0x710);
            for (uint32_t o = 0x490; o <= 0x498; o += 4) put(o, g_oven_up);
            for (uint32_t o = 0x4a0; o <= 0x4a8; o += 4) put(o, g_oven_down);
        }
        if (scn) zero(0x20, 0x20), zero(0x24, 0x24);
        changed = true;
        g_oven_subs.fetch_add(1, std::memory_order_relaxed);
    }
    if (gicut) {
        const float keep = 1.0f - (float)std::clamp(world::live_get(6), 0, 100) * 0.01f;   // (what stays of the game's)
        auto scale = [&](uint32_t a, uint32_t b, float k) {
            for (uint32_t o = a; o <= b && o + 4 <= u->size; o += 4) {
                float v;
                std::memcpy(&v, (uint8_t*)out + o, 4);
                v *= k;
                std::memcpy((uint8_t*)out + o, &v, 4);
            }
        };
        if (env && (cut_bits & 1) && !(cut_bits & 8)) scale(0x460, 0x468, keep);
        if (env && (cut_bits & 2)) scale(0x490, 0x4a8, keep);
        if (env && (cut_bits & 8)) scale(0x440, 0x440, keep);
        if (scn && (cut_bits & 4)) {
            // (cube term = irr x (1 - e) x s20: with e scaled to keep x e, s20 must become keep (1 - e) / (1 - keep e))
            const float e = std::clamp(g_e440.load(std::memory_order_relaxed), 0.0f, 1.0f);
            const float den = 1.0f - ((cut_bits & 8) ? keep : 1.0f) * e;
            scale(0x20, 0x20, den > 1e-3f ? keep * (1.0f - e) / den : 0.0f);
        }
        changed = true;
        g_gicut_subs.fetch_add(1, std::memory_order_relaxed);
    }
    if (own) {
        const float sw = -1.0f;
        std::memcpy((uint8_t*)out + 0x8, &sw, 4);
        changed = true;
        g_own_subs.fetch_add(1, std::memory_order_relaxed);
    }
    if (proj) {
        if (g_sun_proj & 1) std::memset((uint8_t*)out + 0x290, 0, 4);
        if (g_sun_proj & 2) std::memset((uint8_t*)out + 0x294, 0, 4);
        changed = true;
        g_proj_subs.fetch_add(1, std::memory_order_relaxed);
    }
    if (fade) {
        const float far0 = 1.0e8f, far1 = 2.0e8f;
        if (g_sun_fade_mode & 1) std::memcpy((uint8_t*)out + 0x3e0, &far0, 4), std::memcpy((uint8_t*)out + 0x3e4, &far1, 4);
        if (g_sun_fade_mode & 2) std::memcpy((uint8_t*)out + 0x3e8, &far0, 4), std::memcpy((uint8_t*)out + 0x3ec, &far1, 4);
        changed = true;
        g_fade_subs.fetch_add(1, std::memory_order_relaxed);
    }
    if (env && st) {
        if (st == 1 || st == 7) zero(0x50, 0x58), changed = true;
        if (st == 2 || st == 7) zero(0x490, 0x4a8), changed = true;
        if (st == 3 || st == 7) zero(0x440, 0x440), zero(0x460, 0x468), changed = true;
        if (st == 4 || st == 7) zero(0x710, 0x710), changed = true;
    }
    if (scn && st) {
        if (st == 5 || st == 7) zero(0x20, 0x20), changed = true;
        if (st == 6 || st == 7) zero(0x24, 0x24), changed = true;
    }
    if (!changed) return 0;
    if (st) g_term_subs[st].fetch_add(1, std::memory_order_relaxed);
    return u->size;
}

std::string terms_json() {
    std::string s = "{\"every\": " + std::to_string(g_terms_every) + ", \"substitutions\": {";
    for (int i = 0; i < kTermStates; ++i)
        s += std::string(i ? ", " : "") + "\"" + kTermNames[i] + "\": " + std::to_string(g_term_subs[i].load());
    s += "}, \"scn_sizes\": {";
    std::lock_guard lk(g_scn_mu);
    bool first = true;
    for (auto& [k, v] : g_scn_sizes) s += (first ? "\"" : ", \"") + std::to_string(k) + "\": " + std::to_string(v), first = false;
    s += "}, \"env_learnt\": [";
    for (uint32_t i = 0; i < std::min<uint32_t>(g_env_n.load(), 8); ++i) {
        char b[32];
        std::snprintf(b, sizeof b, "%s\"%#llx\"", i ? ", " : "", (unsigned long long)g_env_addr[i]);
        s += b;
    }
    s += "], \"c6\": [";   // (the 12 most used addresses)
    std::vector<std::pair<uint64_t, C6>> v(g_c6.begin(), g_c6.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        char b[200];
        std::snprintf(b, sizeof b, "%s{\"addr\": \"%#llx\", \"count\": %llu, \"size\": [%u, %u], \"sources\": %u, \"refused\": %llu}",
                      i ? ", " : "", (unsigned long long)v[i].first, (unsigned long long)v[i].second.count, v[i].second.min_size,
                      v[i].second.max_size, v[i].second.sources, (unsigned long long)v[i].second.refused);
        s += b;
    }
    return s + "], \"c6_addresses\": " + std::to_string(v.size()) + "}";
}

// rt_uni_dump = N (diagnosis): every uniform block bound during frames N and N+1, in the
// order the host binds them, into uni_dump.bin: per block "UNIB", u32 frame, u32 sequence, u64 program, i32 stage,
// u32 bank, u32 size, u32 kept (bytes that follow, at most 8 KB). rt_world.cpp writes the frame's draws beside it
// (draw_dump.txt) with the same sequence counter.
int64_t g_uni_dump_at = 0, g_uni_dump_at2 = 0;
std::atomic<uint64_t> g_frame_count{0};   // (frames as the core is told of them: the ray world's own count stands still in the VR mode)
std::mutex g_uni_dump_mu;
FILE* g_uni_dump_file = nullptr;
void uni_dump(const RtUniform* u) {
    uint64_t f = world::frame_number();
    if (!f) f = g_frame_count.load(std::memory_order_relaxed);
    // (rt_uni_dump2 = M: two more frames, M and M+1, into uni_dump2.bin: the same run before and after something)
    const bool second = g_uni_dump_at2 && f >= (uint64_t)g_uni_dump_at2 && f <= (uint64_t)g_uni_dump_at2 + 1;
    if ((f < (uint64_t)g_uni_dump_at || f > (uint64_t)g_uni_dump_at + 1) && !second) return;
    std::lock_guard lk(g_uni_dump_mu);
    static FILE* file2 = nullptr;
    if (second && !file2) file2 = std::fopen((out_dir() + "\\uni_dump2.bin").c_str(), "wb");
    if (!second && !g_uni_dump_file) g_uni_dump_file = std::fopen((out_dir() + "\\uni_dump.bin").c_str(), "wb");
    FILE* const file = second ? file2 : g_uni_dump_file;
    if (!file) return;
    const uint32_t kept = u->bytes ? std::min<uint32_t>(u->size, 8192) : 0;
    struct {
        char tag[4];
        uint32_t frame, seq;
        uint64_t key;
        int32_t stage;
        uint32_t bank, size, kept;
    } h{{'U', 'N', 'I', 'B'}, (uint32_t)f, world::dump_seq_next(), u->program_key, u->stage, u->bank, u->size, kept};
    std::fwrite(&h, sizeof h, 1, file);
    if (kept) std::fwrite(u->bytes, 1, kept, file);
    if (f > (uint64_t)(second ? g_uni_dump_at2 : g_uni_dump_at)) std::fflush(file);
}

// rt_cull_mul = "w:f,w:f,..." (diagnosis): in the game's culling context (the block its
// vegetation's sorting passes and task stages read: compute c3 of 600 to 1800 bytes, task c4 of 1600), word w is
// multiplied by f, while our effects are on (key T, RETRACED_AB_EVERY: on and off in one run).
// rt_ctx_mul, the same for gsys_context in the task and mesh stages (c3 of 13568 bytes).
struct WordMul {
    uint32_t w;
    float f;
};
std::vector<WordMul> parse_muls(const char* name) {
    std::vector<WordMul> v;
    std::string s = opt(name, "");
    for (size_t a = 0; a < s.size();) {
        size_t z = s.find(',', a);
        if (z == std::string::npos) z = s.size();
        const std::string t = s.substr(a, z - a);
        const size_t c = t.find(':');
        if (c != std::string::npos) v.push_back({(uint32_t)std::strtoul(t.c_str(), nullptr, 10), (float)std::atof(t.c_str() + c + 1)});
        a = z + 1;
    }
    return v;
}
std::vector<WordMul> g_cull_mul, g_ctx_mul;
std::atomic<uint64_t> g_cull_subs{0}, g_cull_seen{0}, g_cull_nobytes{0};
std::atomic<uint64_t> g_stage_seen[8], g_stage_cull[8], g_stage_nob[8];   // (per NVN stage: blocks seen at all, culling contexts found, blocks without bytes)
// The game's culling context, wherever it is bound (the sorting passes' c3, the task stages' c3 or c4, the mesh stages'
// c4 or c5, cut to what each program reads): told by its content, the camera's position twice (words 16-18 and 20-22,
// words 19 and 23 zero). First tries looked only at the sorting passes and the task stages: the mesh stages, which
// read the same block (and its words 128-133), kept the game's values.
bool is_cull_context(const RtUniform* u) {
    if (!u->bytes || u->size < 112 || u->size > 1800) return false;
    const float* w = (const float*)u->bytes;
    const uint32_t* q = (const uint32_t*)u->bytes;
    return q[19] == 0 && q[23] == 0 && w[16] == w[20] && w[17] == w[21] && w[18] == w[22] && (w[16] != 0 || w[17] != 0 || w[18] != 0);
}
uint32_t cull_substitute(const RtUniform* u, void* out, uint32_t cap) {
    const bool cull = u->stage >= 5 && is_cull_context(u);
    if (u->stage >= 0 && u->stage < 8) {
        g_stage_seen[u->stage].fetch_add(1, std::memory_order_relaxed);
        if (cull) g_stage_cull[u->stage].fetch_add(1, std::memory_order_relaxed);
        if (!u->bytes) g_stage_nob[u->stage].fetch_add(1, std::memory_order_relaxed);
    }
    const bool ctx = (u->stage == 6 || u->stage == 7) && u->bank == 3 && u->size == 13568;
    if (cull || ctx) g_cull_seen.fetch_add(1, std::memory_order_relaxed);
    if ((cull || ctx) && !u->bytes) g_cull_nobytes.fetch_add(1, std::memory_order_relaxed);
    if (!u->bytes || u->size > cap || !g_effects_on.load(std::memory_order_relaxed)) return 0;
    const std::vector<WordMul>& m = cull ? g_cull_mul : ctx ? g_ctx_mul : g_cull_mul;
    if (!(cull || ctx) || m.empty()) return 0;
    std::memcpy(out, u->bytes, u->size);
    for (const WordMul& x : m) {
        if ((x.w + 1) * 4 > u->size) continue;
        float v;
        std::memcpy(&v, (uint8_t*)out + x.w * 4, 4);
        v *= x.f;
        std::memcpy((uint8_t*)out + x.w * 4, &v, 4);
    }
    g_cull_subs.fetch_add(1, std::memory_order_relaxed);
    return u->size;
}

uint32_t rt_uniform_seen(const RtUniform* u, void* out, uint32_t cap) {
    if (!u) return 0;
    if (g_uni_dump_at) uni_dump(u);   // (diagnosis: every block as the game gives it, in the VR mode too)
    if (vr::wanted()) {   // (the VR mode's own blocks first: Link's pose, what he holds)
        if (const uint32_t n = vr::uniform_seen(u, out, cap)) return n;
        if (!vr::effects()) return 0;
    }
    if ((!g_cull_mul.empty() || !g_ctx_mul.empty()) && u->stage >= 5)
        if (const uint32_t n = cull_substitute(u, out, cap)) return n;
    if (g_terms || g_sun_fade || g_sun_proj || g_oven || g_sun_own || g_gi_cut) {
        env_learn(u);
        if (std::find(watched.begin(), watched.end(), u->program_key) == watched.end())
            return terms_substitute(u, out, cap);   // (every program: not recorded, only the terms)
    }
    if (u->program_key == kFogApply && u->stage == 1 && u->bank == 4 && u->size >= 0x17c && u->bytes)
        world::set_sun((const float*)((const uint8_t*)u->bytes + 0x170)), world::set_env((const uint8_t*)u->bytes, u->size);   // (the sun's direction for our rays; the cube's scale)
    {
        std::lock_guard lk(uni_mu);
        Seen& v = seen[{u->program_key, u->stage, u->bank}];
        v.count++;
        v.source = u->bytes ? u->source : 2;
        v.size = u->size;
        if (u->bytes) std::memcpy(v.bytes, u->bytes, std::min<size_t>(u->size, sizeof v.bytes));
    }
    const bool on = g_effects_on.load(std::memory_order_relaxed);
    const bool ours = on && g_curve_patched.load(std::memory_order_relaxed);   // (our curve is in the program)
    const int strength = ours ? 0 : std::clamp(g_hdr_strength.load(std::memory_order_relaxed), 0, 3);
    if (g_hdr_curve_watch && (g_full_peak || !on || ours || strength > 0) && u->program_key == kGameHdrCurve &&
        u->stage == 1 && u->bank == 3 && u->bytes && u->size >= 0x4c && u->size <= cap) {
        std::memcpy(out, u->bytes, u->size);
        float r;
        std::memcpy(&r, (const uint8_t*)u->bytes + 0x40, 4);
        if (!on) r = 1.0f;   // (P = 1: the expansion is the identity, the game's SDR picture at paper white)
        else if (g_primary.peak_nits > 0 && g_primary.paper_white_nits > 0 && (g_full_peak || ours || strength > 0))
            r = (float)(g_primary.peak_nits / g_primary.paper_white_nits);
        std::memcpy((uint8_t*)out + 0x40, &r, 4);
        if (ours) {   // (Nintendo's luminance expansion switched off: our curve already reaches the peak)
            const float never = 1e9f;
            std::memcpy((uint8_t*)out + 0x44, &never, 4);
        } else if (on && strength > 0) {   // (the game's own formula, with our knee and shape)
            static const float kK[4] = {0.65f, 0.45f, 0.30f, 0.20f}, kG[4] = {0.75f, 1.5f, 2.5f, 3.0f};
            std::memcpy((uint8_t*)out + 0x44, &kK[strength], 4);
            std::memcpy((uint8_t*)out + 0x48, &kG[strength], 4);
        }
        g_substituted.fetch_add(1, std::memory_order_relaxed);
        return u->size;
    }
    return 0;
}

void write_report(const char* why) {
    char b[32768];   // (the options read are listed here: several kilobytes once the VR mode and our effects both run)
    size_t unique;
    {
        std::lock_guard lk(S.unique_mu);
        unique = S.unique.size();
    }
    const double frame_us_avg = S.frames ? S.frame_ns_total / 1000.0 / S.frames : 0;
    const double spirv_ms = S.spirv_ns.load() / 1e6;
    const double t_dev = S.t_device ? (S.t_device - S.t_init) / 1e9 : -1;
    const double t_ff = S.t_first_frame ? (S.t_first_frame - S.t_init) / 1e9 : -1;
    std::snprintf(b, sizeof b,
        "{\n"
        "  \"api_version\": %u,\n"
        "  \"core_build\": \"%s\",\n"
        "  \"host\": \"%s\",\n"
        "  \"host_revision\": \"%s\",\n"
        "  \"written_because\": \"%s\",\n"
        "  \"active\": %s,\n"
        "  \"gpu\": {\"name\": \"%s\", \"driver_version\": %u, \"api_version\": \"%u.%u.%u\"},\n"
        "  \"seconds_to_device\": %.3f,\n"
        "  \"seconds_to_first_frame\": %.3f,\n"
        "  \"frames\": %llu,\n"
        "  \"fps_recent\": %.2f,\n"
        "  \"shader_modules\": {\"seen\": %llu, \"unique\": %zu, \"patched\": %llu, \"mwords\": %.3f, \"ms_in_core\": %.3f},\n"
        "  \"cpu_in_frame_hook\": {\"us_avg\": %.3f, \"us_max\": %.3f, \"report_write_us_max\": %.1f},\n"
        "  \"options\": %s,\n"
        "  \"uniforms\": ",
        RT_API_VERSION, RT_CORE_BUILD, json_escape(S.host_name).c_str(), json_escape(S.host_revision).c_str(), why,
        S.active ? "true" : "false", json_escape(S.gpu_name).c_str(), S.driver_version,
        VK_API_VERSION_MAJOR(S.api_version), VK_API_VERSION_MINOR(S.api_version), VK_API_VERSION_PATCH(S.api_version),
        t_dev, t_ff, (unsigned long long)S.frames, S.fps_window,
        (unsigned long long)S.modules.load(), unique, (unsigned long long)S.modules_patched.load(),
        S.words.load() / 1e6, spirv_ms, frame_us_avg, S.frame_ns_max / 1000.0, S.report_ns_max / 1000.0,
        options_json().c_str());
    const std::string tail = ",\n  \"displays\": " + S.displays + ",\n  \"uniforms_substituted\": " +
                             std::to_string(g_substituted.load()) + ",\n  \"effects_on\": " +
                             (g_effects_on.load() ? "true" : "false") + ",\n  \"effects_toggles\": " +
                             std::to_string(g_toggles.load()) + ",\n  \"status_line\": \"" + std::string(world::status_text()) + "\",\n  \"menu\": [" +
                             std::to_string(world::live_get(0)) + ", " + std::to_string(world::live_get(1)) + ", " + std::to_string(world::live_get(2)) + ", " +
                             std::to_string(world::live_get(3)) + ", " + std::to_string(world::live_get(4)) + "],\n  \"hdr_strength\": " + std::to_string(g_hdr_strength.load()) +
                             ",\n  \"sun_fade_substitutions\": " + std::to_string(g_fade_subs.load()) + ",\n  \"sun_proj_substitutions\": " + std::to_string(g_proj_subs.load()) + ",\n  \"oven_substitutions\": " + std::to_string(g_oven_subs.load()) + ",\n  \"sun_own_substitutions\": " + std::to_string(g_own_subs.load()) + ",\n  \"gi_cut_substitutions\": " + std::to_string(g_gicut_subs.load()) + ",\n  \"cull_blocks\": [" + std::to_string(g_cull_seen.load()) + ", " + std::to_string(g_cull_nobytes.load()) + ", " + std::to_string(g_cull_subs.load()) + "], \"cull_by_stage\": [" + [] {
                                 std::string s;
                                 for (int i = 5; i < 8; ++i)
                                     s += (i > 5 ? ", [" : "[") + std::to_string(g_stage_seen[i].load()) + ", " + std::to_string(g_stage_cull[i].load()) + ", " + std::to_string(g_stage_nob[i].load()) + "]";
                                 return s;
                             }() + "]" + ",\n  \"light_terms\": " + terms_json() + ",\n  \"vertex_modules_edited\": " + std::to_string(g_xfb_patched.load()) + ",\n  \"gbuf_programs\": {\"listed\": " + std::to_string(g_gbuf_files.size()) + ", \"read\": " + std::to_string(g_gbuf_read.load()) +
                             ", \"megabytes\": " + std::to_string(g_gbuf_bytes.load() >> 20) + "},\n  \"gbuf\": " + gbuf::report_json() + ",\n  \"vegetation\": " + veg::report_json() + ",\n  \"world\": " + world::report_json() + ",\n  \"vr\": " + vr::report_json() + ",\n  \"xr\": " + xr::report_json() + "\n}\n";
    // (the file write takes about 1 ms on Windows: on a thread of its own, never on the presentation thread)
    std::thread([text = std::string(b) + uniforms_json() + tail] {
        static std::mutex write_mu;
        std::lock_guard lk(write_mu);
        if (!write_file_atomic(out_dir() + "\\rt_report.json", text)) log("report: could not write %s", out_dir().c_str());
    }).detach();
}

// ---- entries ----

int rt_init(const RtHost* h) {
    if (!h || h->version != RT_API_VERSION) return 0;
    host_log_fn = h->log;
    g_host = h;
    S.t_init = now_ns();
    S.host_name = h->host_name ? h->host_name : "?";
    S.host_revision = h->host_revision ? h->host_revision : "?";
    load_options();
    vr::configure();   // (before anything reads an option: the VR mode switches our graphics effects off)
    if (opt_int("enabled", 1) == 0) {
        log_host("core %s: disabled by retraced.ini / RETRACED_ENABLED=0", RT_CORE_BUILD);
        S.active = false;
        write_report("disabled");
        return 0;
    }
    {   // watch_uniforms = ab3681946f93594a,... : programs whose uniform blocks go into the report
        std::string w = opt("watch_uniforms", "");
        for (size_t a = 0; a < w.size();) {
            size_t z = w.find(',', a);
            if (z == std::string::npos) z = w.size();
            std::string t = w.substr(a, z - a);
            if (!t.empty()) watched.push_back(std::strtoull(t.c_str(), nullptr, 16));
            a = z + 1;
        }
    }
    {   // the displays as Windows sees them; with hdr=1, the game is told the real peak and paper white (hooks X1, X2 read
        // RETRACED_GAME_HDR*: set here unless the user gave them; the host shares this process's C runtime environment)
        auto ds = displays();
        S.displays = displays_json(ds);
        for (auto& d : ds)
            log_host("display %s%s: %dx%d, HDR %s, %u bits, peak %.0f nits, full frame %.0f, paper white %.0f", d.name.c_str(),
                     d.primary ? " (primary)" : "", d.width, d.height, d.hdr_on ? "on" : "off", d.bits, d.peak_nits,
                     d.full_frame_nits, d.paper_white_nits);
        if (opt_int("hdr", 0) != 0) {
            const DisplayInfo* p = nullptr;
            for (auto& d : ds) if (d.primary) p = &d;
            if (!p && !ds.empty()) p = &ds[0];
            if (p) g_primary = *p;
            g_hdr_curve_watch = true;
            g_hdr_strength.store((int)std::clamp<int64_t>(opt_int("hdr_strength", 1), 0, 3));
            if (std::find(watched.begin(), watched.end(), kGameHdrCurve) == watched.end()) watched.push_back(kGameHdrCurve);
            if (opt_int("hdr_full_peak", 0) != 0 && p && p->peak_nits > 0 && p->paper_white_nits > 0) {
                g_full_peak = true;
                log_host("hdr: the game's curve will reach the display's peak (%.0f nits, c3[0x40] = %.3f)", p->peak_nits,
                         p->peak_nits / p->paper_white_nits);
            }
            auto set = [](const char* k, const std::string& v) {
                if (!std::getenv(k)) { _putenv_s(k, v.c_str()); log_host("hdr: %s=%s given to the game", k, v.c_str()); }
            };
            set("RETRACED_GAME_HDR", "1");
            if (p && p->peak_nits > 0) set("RETRACED_GAME_HDR_PEAK", std::to_string((int)(p->peak_nits + 0.5)));
            if (p && p->paper_white_nits > 0) set("RETRACED_GAME_HDR_PAPER", std::to_string((int)(p->paper_white_nits + 0.5)));
            if (p && !p->hdr_on) log_host("hdr: Windows HDR is OFF on %s: the picture will be converted back to SDR", p->name.c_str());
        }
    }
    if (world::sun_wanted() || world::refl_wanted() || world::air_wanted()) watched.push_back(kFogApply);   // (the sun's direction: where the fog program binds the environment)
    g_sun_fade_mode = opt_int("rt_sun_fade", 1);
    g_sun_fade = world::sun_wanted() && g_sun_fade_mode != 0;
    g_sun_proj = world::sun_wanted() ? opt_int("rt_sun_proj", 0) : 0;
    if (g_sun_fade) log_host("sun: the mask's sun shadow kept at every distance, the baked one left out (rt_sun_fade=%lld)", (long long)g_sun_fade_mode);
    if (const int64_t k = opt_int("light_oven", 0); k > 0) {
        g_oven = true;
        g_oven_up = (float)k / 1000.0f;
        g_oven_down = (float)opt_int("light_oven_down", k) / 1000.0f;
        log_host("light oven on: hemisphere up %.3f, down %.3f, every other light of the lit programs cut", g_oven_up, g_oven_down);
    }
    // (default 0: in the gorge of the plain the rays alone let the sun in where the game has a
    // deep shadow, cast by cliffs the rays do not know (outside the view). Until the ray world holds what is out of
    // sight, the safe rule stands: our shadows are added, none of the game's is lost.)
    g_sun_own = world::sun_wanted() && gbuf::wanted() && opt_int("rt_sun_own", 0) != 0;
    if (g_sun_own) log_host("sun: in our lit programs the sun is limited by the mask's G channel alone (rt_sun_own=1)");
    // (7: the probes' tint, the hemisphere, the cube's ambient. It was 14, with env[0x440] taken to zero
    // instead of the tint, to cut the lightmaps and the terrain's baked light too (the forest). But env[0x440] also
    // weighs the game's own darkening of its reflections where its baked light is dark (anatomie-eclairage.md, 4.6:
    // specAmb x (1 + (ratio - 1) env[0x440])): at zero, the wet stones under the fountain's bowl and in its basin
    // mirrored the whole sky and went blinding white, and with them the water over them. Three trials at the same
    // view: 14 white, 7 as the game, 0 as the game. To cut the baked light of the lightmaps and the terrain
    // again, the programs that carry that reflection term must first be told apart, one by one.)
    g_gi_cut_bits = opt_int("rt_gi_cut", 7);
    g_gi_cut = world::gi_wanted() && g_gi_cut_bits != 0 && opt_int("rt_light", 1) != 0;   // (rt_light=0: not even made ready)
    if (g_gi_cut) log_host("light: the game's ambient light cut in our lit programs while the light by rays is on (rt_gi_cut=%lld)", (long long)g_gi_cut_bits);
    // (the light by rays alone only changes the environment and scene blocks of our lit programs' fragment stage: the
    // host is asked for those alone (~1); the other substitutions read other blocks: every block (~0).
    // rt_watch_all = 1: every block, as before.)
    if ((g_sun_fade || g_sun_proj || g_oven || g_sun_own || g_gi_cut) && opt_int("light_terms", 0) == 0)
        watched.push_back(g_sun_fade || g_sun_proj || g_oven || g_sun_own || opt_int("rt_watch_all", 0) != 0 ? ~0ull : ~1ull);
    // (the environment block is learnt where the fog program binds it: asked for by name, whatever else is)
    if (g_gi_cut && std::find(watched.begin(), watched.end(), kFogApply) == watched.end()) watched.push_back(kFogApply);
    g_uni_dump_at = opt_int("rt_uni_dump", 0), g_uni_dump_at2 = opt_int("rt_uni_dump2", 0);
    g_cull_mul = parse_muls("rt_cull_mul"), g_ctx_mul = parse_muls("rt_ctx_mul");
    if (!g_cull_mul.empty() || !g_ctx_mul.empty()) {
        watched.push_back(~0ull);
        log_host("grass: culling context words multiplied (%zu) and gsys_context words (%zu), while the effects are on", g_cull_mul.size(), g_ctx_mul.size());
    }
    if (g_uni_dump_at) watched.push_back(~0ull);   // (every program's blocks: diagnosis only)
    if (opt_int("light_terms", 0) != 0) {
        g_terms = true;
        g_terms_every = std::max<int64_t>(30, opt_int("light_terms_every", 240));
        watched.push_back(~0ull);
        log_host("light terms cycle on: %d states of %lld frames", kTermStates, (long long)g_terms_every);
    }
    {   // SPIR-V: our patches, and the optional dump of modules holding given words
        load_patches();
        std::string sig = opt("spirv_dump_sig", "");
        for (size_t a = 0; a < sig.size();) {
            size_t z = sig.find(',', a);
            if (z == std::string::npos) z = sig.size();
            if (z > a) g_spv_sig.push_back((uint32_t)std::strtoul(sig.substr(a, z - a).c_str(), nullptr, 16));
            a = z + 1;
        }
    }
    g_effects_on.store(opt_int("effects", 1) != 0);   // (effects = 0: starts with our effects off; key 2 flips)
    S.report_every = opt_int("report_every", 300);
    if (S.report_every < 30) S.report_every = 30;
    S.active = true;
    log_host("core %s (api %u) active, host %s %s, outputs in %s", RT_CORE_BUILD, RT_API_VERSION, S.host_name.c_str(),
             S.host_revision.c_str(), out_dir().c_str());
    write_report("init");
    return 1;
}

// ---- HDR10 window: option hdr = 1 asks for an A2B10G10R10 + ST 2084 swapchain. The game, told the display
// is HDR (RETRACED_GAME_HDR=1, hook X1), then writes its own PQ-encoded 10-bit picture, which the present copies as is.
bool hdr_wanted() { return opt_int("hdr", 0) != 0; }

uint32_t rt_instance_extensions(const char* const* avail, uint32_t n, const char** out, uint32_t cap) {
    uint32_t k = 0;
    if (hdr_wanted())
        for (uint32_t i = 0; i < n && k < cap; ++i)
            if (std::strcmp(avail[i], VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) out[k++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
    if (hdr_wanted()) log_host("hdr: %s", k ? "VK_EXT_swapchain_colorspace asked" : "VK_EXT_swapchain_colorspace missing");
    if (k < cap) k += xr::instance_extensions(avail, n, out + k, cap - k);   // (what the headset's runtime asks of Vulkan)
    return k;
}

int rt_swapchain_format(const VkSurfaceFormatKHR* f, uint32_t n, VkSurfaceFormatKHR* pick) {
    if (!hdr_wanted()) return 0;
    for (uint32_t i = 0; i < n; ++i)
        if (f[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            *pick = f[i];
            log_host("hdr: swapchain A2B10G10R10 + HDR10 ST 2084 chosen");
            return 1;
        }
    std::string list;
    for (uint32_t i = 0; i < n; ++i) list += std::to_string(f[i].format) + "/" + std::to_string(f[i].colorSpace) + " ";
    log_host("hdr: no A2B10G10R10 + ST 2084 surface format (formats: %s), SDR kept", list.c_str());
    return 0;
}
uint32_t rt_device_extensions(const char* const* avail, uint32_t n, const char** out, uint32_t cap) {
    uint32_t k = 0;
    if (hdr_wanted())
        for (uint32_t i = 0; i < n && k < cap; ++i)
            if (std::strcmp(avail[i], VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0) out[k++] = VK_EXT_HDR_METADATA_EXTENSION_NAME, g_hdr_metadata = true;
    if (k < cap) k += world::device_extensions(avail, n, out + k, cap - k);
    if (k < cap) k += xr::device_extensions(avail, n, out + k, cap - k);
    return k;
}

// The HDR10 metadata sent with the swapchain: BT.2020 primaries, D65 white, the display's peak and black, the content's
// brightest pixel = the game's own cap (3.5 x paper white, measured) and the display's full-frame level.
bool g_hdr_window = false;   // (the window's swapchain is A2B10G10R10 + HDR10 ST 2084)

// sRGB 8-bit -> BT.2020 linear at paper white -> PQ, packed A2B10G10R10 (R in bits 0-9). Formulas from the public
// standards (IEC 61966-2-1 for sRGB, ITU-R BT.2087 for the 709 -> 2020 matrix, SMPTE ST 2084 for PQ).
void rt_overlay_pixels(uint8_t* p, size_t n) {
    if (!g_hdr_window || !p) return;
    static float lin[256];
    static uint16_t pq[4096];   // (linear 0..1 of paper white, in 4096 steps -> 10-bit PQ code)
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) {
            const double e = i / 255.0;
            lin[i] = (float)(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
        }
        const double white = g_primary.paper_white_nits > 0 ? g_primary.paper_white_nits : 203.0;
        for (int i = 0; i < 4096; ++i) {
            const double y = (i / 4095.0) * white / 10000.0;
            const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
            const double ym = std::pow(y, m1);
            pq[i] = (uint16_t)std::lround(std::pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2) * 1023.0);
        }
        ready = true;
    }
    auto code = [](float v) { return pq[std::clamp((int)(v * 4095.0f + 0.5f), 0, 4095)]; };
    for (size_t i = 0; i < n; ++i, p += 4) {
        const float b = lin[p[0]], g = lin[p[1]], r = lin[p[2]];   // (GDI's order: B, G, R, A)
        const float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
        const float g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
        const float b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
        const uint32_t w = (uint32_t)code(r2) | ((uint32_t)code(g2) << 10) | ((uint32_t)code(b2) << 20) | (3u << 30);
        std::memcpy(p, &w, 4);
    }
}

void rt_swapchain_created(VkSwapchainKHR sc, VkSurfaceFormatKHR fmt) {
    g_hdr_window = fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    if (!g_hdr_metadata || fmt.colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT || !g_gdpa || g_primary.peak_nits <= 0) return;
    auto set = (PFN_vkSetHdrMetadataEXT)g_gdpa(g_device, "vkSetHdrMetadataEXT");
    if (!set) return;
    VkHdrMetadataEXT m{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    m.displayPrimaryRed = {0.708f, 0.292f};
    m.displayPrimaryGreen = {0.170f, 0.797f};
    m.displayPrimaryBlue = {0.131f, 0.046f};
    m.whitePoint = {0.3127f, 0.3290f};
    m.maxLuminance = (float)g_primary.peak_nits;
    m.minLuminance = (float)g_primary.min_nits;
    const double cap = (g_full_peak || g_hdr_strength.load() > 0 || g_primary.paper_white_nits <= 0) ? g_primary.peak_nits : 3.5 * g_primary.paper_white_nits;
    m.maxContentLightLevel = (float)std::min(cap, g_primary.peak_nits);
    m.maxFrameAverageLightLevel = (float)std::min(g_primary.full_frame_nits > 0 ? g_primary.full_frame_nits : cap, cap);
    set(g_device, 1, &sc, &m);
    log_host("hdr: metadata sent (display %.0f nits, content up to %.0f, frame average %.0f)", m.maxLuminance,
             m.maxContentLightLevel, m.maxFrameAverageLightLevel);
}
void rt_device_features(void*** tail) { world::device_features(tail); }

int rt_wants_draws() { return world::wanted() || vr::wanted() ? 1 : 0; }
int rt_draw(const RtDraw* d) {
    if (!vr::wanted()) return world::draw(d);
    const int v = vr::draw(d);
    if (!vr::effects()) return v;
    // (both: the VR mode's own needs and our effects'. A draw the VR mode gives other triangles for is not captured
    // for the rays: the capture counts on the draw's own triangles.)
    int w = world::draw(d) & 3;
    if (v & 4) w &= ~2;
    return v | w;
}
int rt_live_get(int id) { return id >= 13 ? vr::live_get(id) : world::live_get(id); }
void rt_live_set(int id, int value) {
    if (id >= 13) vr::live_set(id, value);
    else world::live_set(id, value);
}
const char* rt_status_text() { return world::status_text(); }
void rt_inject(VkCommandBuffer c) {
    const bool vr_did = vr::inject(c);
    if (!vr_did || vr::effects()) world::inject(c);
}
void rt_recording() {
    world::recording();
    vr::recording();
}
int rt_wants_targets() { return gbuf::wanted() ? 1 : 0; }
uint32_t rt_extra_targets(const RtTargets* in, VkImageView* views, VkFormat* formats) { return gbuf::extra_targets(in, views, formats); }

int rt_wants_ui() { return vr::wants_ui() ? 1 : 0; }
int rt_ui_target(VkImage image, VkFormat format, VkImageUsageFlags usage, uint32_t w, uint32_t h, int has_depth, VkImageView* view,
                 VkFormat* view_format) {
    return vr::ui_target(image, format, usage, w, h, has_depth, view, view_format);
}
int rt_ui_view(VkImageView v) { return vr::ui_view(v); }
const uint32_t* rt_draw_indices(uint32_t* n) { return vr::draw_indices(n); }

void rt_device_created(const RtDevice* d) {
    world::set_effects([] { return g_effects_on.load(std::memory_order_relaxed); });
    if (d && g_host) world::device_created(g_host, d);
    if (d && g_host && world::ready()) veg::device_created();
    gbuf::device_created();
    if (d && g_host) vr::device_created(g_host, d);
    if (d && g_host) xr::device_created(g_host, d);
    if (d) g_device = d->device, g_gdpa = d->get_device_proc;
    S.t_device = now_ns();
    if (d && d->get_instance_proc && d->physical) {
        auto props = (PFN_vkGetPhysicalDeviceProperties)d->get_instance_proc(d->instance, "vkGetPhysicalDeviceProperties");
        if (props) {
            VkPhysicalDeviceProperties p{};
            props(d->physical, &p);
            S.gpu_name = p.deviceName;
            S.driver_version = p.driverVersion;
            S.api_version = p.apiVersion;
        }
    }
    log_host("device: %s, queue family %u", S.gpu_name.c_str(), d ? d->queue_family : 0);
}

int rt_spirv_patch(const uint32_t* words, size_t n, const uint32_t** out, size_t* out_n) {
    const int64_t t0 = now_ns();
    const uint64_t h = fnv1a64(words, n);
    {
        std::lock_guard lk(S.unique_mu);
        S.unique.insert(h);
    }
    S.modules.fetch_add(1, std::memory_order_relaxed);
    S.words.fetch_add(n, std::memory_order_relaxed);
    if (!g_spv_sig.empty()) spirv_dump_if_match(h, words, n);
    int replaced = 0;
    if (auto it = g_patches.find(h); it != g_patches.end()) {   // (read-only after init: safe on any thread)
        *out = it->second.words.data();
        *out_n = it->second.words.size();
        replaced = it->second.gbuf ? 3 : 1;
        S.modules_patched.fetch_add(1, std::memory_order_relaxed);
        if (it->second.curve) g_curve_patched.store(true);
        if (S.modules_patched.load(std::memory_order_relaxed) <= 8)
            log_host("spirv: module %016llx replaced by our patch (%zu -> %zu words)", (unsigned long long)h, n, *out_n);
    }
    if (!replaced && !g_gbuf_files.empty()) {
        if (auto it = g_gbuf_files.find(h); it != g_gbuf_files.end()) {
            if (FILE* in = std::fopen(it->second.c_str(), "rb")) {
                std::fseek(in, 0, SEEK_END);
                const long bytes = std::ftell(in);
                std::fseek(in, 0, SEEK_SET);
                auto* v = new std::vector<uint32_t>(bytes > 0 ? (size_t)bytes / 4 : 0);
                const size_t got = v->empty() ? 0 : std::fread(v->data(), 4, v->size(), in);
                std::fclose(in);
                if (got == v->size() && got > 5 && (*v)[0] == 0x07230203u) {
                    {
                        std::lock_guard lk(g_xfb_mu);
                        g_xfb_live[v->data()] = v;   // (freed by spirv_release, as the modules edited on the fly)
                    }
                    *out = v->data();
                    *out_n = v->size();
                    replaced = g_gbuf_sun.count(h) ? 7 : 3;   // (bit 1: our per-pixel outputs; bit 2: the sun switch)
                    S.modules_patched.fetch_add(1, std::memory_order_relaxed);
                    g_gbuf_read.fetch_add(1, std::memory_order_relaxed);
                    g_gbuf_bytes.fetch_add((uint64_t)bytes, std::memory_order_relaxed);
                } else {
                    delete v;
                }
            }
        }
    }
    if (uint64_t au = 0, af = 0; !replaced && veg::addresses(&au, &af, &g_veg_slots)) {   // (grass and foliage: rt_veg.h)
        auto* v = new std::vector<uint32_t>;
        const int r = spv_add_mesh_capture(words, n, au, af, g_veg_slots, *v);
        veg::note_module(r);
        if (r == 1) {
            {
                std::lock_guard lk(g_xfb_mu);
                g_xfb_live[v->data()] = v;
            }
            static std::atomic<int64_t> dumped{0};
            static const int64_t dump = opt_int("spirv_dump_veg", 0);
            if (dumped.fetch_add(1, std::memory_order_relaxed) < dump) {   // (the first ones, before and after, for spirv-val)
                const std::string dir = out_dir() + "\\spv";
                CreateDirectoryA(dir.c_str(), nullptr);
                char name[80];
                std::snprintf(name, sizeof name, "\\veg_%016llx", (unsigned long long)h);
                if (FILE* o = std::fopen((dir + name + "_avant.spv").c_str(), "wb")) std::fwrite(words, 4, n, o), std::fclose(o);
                if (FILE* o = std::fopen((dir + name + "_apres.spv").c_str(), "wb")) std::fwrite(v->data(), 4, v->size(), o), std::fclose(o);
            }
            *out = v->data();
            *out_n = v->size();
            replaced = 1;
        } else {
            delete v;
        }
    }
    if (!replaced && world::xfb_patching()) {   // (every vertex module gives its positions: rt_spv.cpp)
        auto* v = new std::vector<uint32_t>;
        const int r = spv_add_position_xfb(words, n, *v);
        if (r == 1) {
            {
                std::lock_guard lk(g_xfb_mu);
                g_xfb_live[v->data()] = v;
            }
            const uint64_t k = g_xfb_patched.fetch_add(1, std::memory_order_relaxed);
            static const int64_t dump = opt_int("spirv_dump_xfb", 0);
            if ((int64_t)k < dump) {   // (the first ones, before and after, for spirv-val)
                const std::string dir = out_dir() + "\\spv";
                CreateDirectoryA(dir.c_str(), nullptr);
                char name[80];
                std::snprintf(name, sizeof name, "\\xfb_%016llx", (unsigned long long)h);
                if (FILE* o = std::fopen((dir + name + "_avant.spv").c_str(), "wb")) std::fwrite(words, 4, n, o), std::fclose(o);
                if (FILE* o = std::fopen((dir + name + "_apres.spv").c_str(), "wb")) std::fwrite(v->data(), 4, v->size(), o), std::fclose(o);
            }
            *out = v->data();
            *out_n = v->size();
            replaced = 1;
        } else {
            delete v;
            if (r < 0) world::xfb_patch_failed();
        }
    }
    S.spirv_ns.fetch_add((uint64_t)(now_ns() - t0), std::memory_order_relaxed);
    return replaced;
}

void rt_spirv_release(const uint32_t* out) {
    std::vector<uint32_t>* v = nullptr;
    {
        std::lock_guard lk(g_xfb_mu);
        if (auto it = g_xfb_live.find(out); it != g_xfb_live.end()) v = it->second, g_xfb_live.erase(it);
    }
    delete v;   // (a module from the patch table is not ours to free: not in the map)
}

void rt_xfb_begin(VkCommandBuffer c) { world::xfb_begin(c); }
void rt_xfb_end(VkCommandBuffer c) { world::xfb_end(c); }

// The ² key (VK_OEM_7 on a French keyboard; option toggle_key = another virtual-key code), read once a frame while a
// window of this process is in front.
void poll_toggle() {
    // ab_every = N: the switch flips by itself every N frames (A/B inside one run)
    static const int64_t ab = opt_int("ab_every", 0);
    if (ab > 0 && S.frames > 0 && S.frames % (uint64_t)ab == 0) {
        g_effects_on.store(!g_effects_on.load());
        g_toggles.fetch_add(1);
    }
    // two keys: ² (VK_OEM_7 on a French keyboard) and T, as in the Breath of the Wild project (the game leaves T free);
    // toggle_key / toggle_key2 = other virtual-key codes, 0 = that key unused
    static const int vk = (int)opt_int("toggle_key", VK_OEM_7), vk2 = (int)opt_int("toggle_key2", 'T');
    static bool was_down = false;
    const bool down = (vk && (GetAsyncKeyState(vk) & 0x8000) != 0) || (vk2 && (GetAsyncKeyState(vk2) & 0x8000) != 0);
    if (down && !was_down) {
        DWORD pid = 0;
        if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) {
            const bool on = !g_effects_on.load();
            g_effects_on.store(on);
            g_toggles.fetch_add(1);
            log_host("effects %s (key)", on ? "ON" : "OFF");
            // (a short beep, high = on, low = off: the picture may hardly change, the key must still be felt to work;
            // on a thread of its own, Beep waits for the sound. toggle_beep = 0: silent)
            if (opt_int("toggle_beep", 1) != 0) std::thread([on] { Beep(on ? 1320 : 660, 70); }).detach();
        }
    }
    was_down = down;
    // Longshot's own menu opens with Insert or F1: a small keyboard may have neither. menu_key (M) presses Insert for
    // the player, when the game's window is in front (0: unused).
    static const int vkm = (int)opt_int("menu_key", 'M');
    static bool was_m = false;
    const bool dm = vkm && (GetAsyncKeyState(vkm) & 0x8000) != 0;
    if (dm && !was_m) {
        DWORD pid = 0;
        if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) {
            keybd_event(VK_INSERT, 0, 0, 0);
            keybd_event(VK_INSERT, 0, KEYEVENTF_KEYUP, 0);
            log_host("menu key: Insert pressed for the player");
        }
    }
    was_m = dm;
    static const int vk3 = (int)opt_int("strength_key", 'Y');
    static bool was3 = false;
    const bool d3 = vk3 && g_hdr_curve_watch && (GetAsyncKeyState(vk3) & 0x8000) != 0;
    if (d3 && !was3) {
        DWORD pid = 0;
        if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) {
            const int s = (g_hdr_strength.load() + 1) % 4;
            g_hdr_strength.store(s);
            log_host("hdr strength %d (key)", s);
            if (opt_int("toggle_beep", 1) != 0)
                std::thread([s] {
                    if (!s) Beep(440, 220);
                    for (int i = 0; i < s; ++i) Beep(1320, 60), Sleep(70);
                }).detach();
        }
    }
    was3 = d3;
}

uint32_t rt_must_draw_programs(uint64_t* keys, uint32_t cap) {
    // the end-of-frame draws, SDR and HDR (curves, final copy / PQ encode): waited for, never left out
    // (and, in the VR mode, the interface's two programs: drawn into our image, their pipelines are new ones)
    // (and the tone curve's variant with the sun's glare, a00726fca31a4039: met when the sun enters the view)
    static const uint64_t k[] = {0xab3681946f93594aull, 0xb7914ae65792d0c8ull, kGameHdrCurve, 0xdbb1015dd9fcec75ull,
                                 0xfc10fab78898d67eull, 0xe86f9970e0324e77ull, 0xa00726fca31a4039ull};
    uint32_t n = 0;
    for (uint64_t v : k)
        if (n < cap) keys[n++] = v;
    return n;
}

void rt_frame(const RtFrame* f) {
    g_frame_count.fetch_add(1, std::memory_order_relaxed);
    vr::frame(f ? f->number : 0);
    world::frame();
    gbuf::frame();
    poll_toggle();
    int64_t t0 = now_ns();
    S.frames = f ? f->number : S.frames + 1;
    if (!S.t_first_frame) {
        S.t_first_frame = t0;
        S.t_last_report = t0;
        log_host("first frame after %.1f s", (t0 - S.t_init) / 1e9);
    }
    if (S.frames >= S.next_report) {
        const int64_t dt = t0 - S.t_last_report;
        if (dt > 0) S.fps_window = (S.frames - S.frames_at_last_report) * 1e9 / dt;
        S.t_last_report = t0;
        S.frames_at_last_report = S.frames;
        S.next_report = S.frames + (uint64_t)S.report_every;
        const int64_t r0 = now_ns();
        write_report("periodic");
        const int64_t rn = now_ns() - r0;
        if (rn > S.report_ns_max) S.report_ns_max = rn;
        t0 += rn;   // (the hook's own cost, without the report's file write)
    }
    const int64_t dt = now_ns() - t0;
    S.frame_ns_total += dt;
    if (dt > S.frame_ns_max && S.frames > 120) S.frame_ns_max = dt;   // (the first reports are written at start-up)
}

const RtExports exports = {
    RT_API_VERSION, (uint32_t)sizeof(RtExports),
    rt_init, rt_instance_extensions, rt_device_extensions, rt_device_features, rt_device_created,
    rt_spirv_patch, rt_spirv_release, rt_frame, rt_watched_programs, rt_uniform_seen,
    rt_swapchain_format, rt_swapchain_created, rt_must_draw_programs, rt_overlay_pixels, rt_wants_draws, rt_draw, rt_inject,
    rt_recording, rt_xfb_begin, rt_xfb_end, rt_wants_targets, rt_extra_targets,
    rt_live_get, rt_live_set, rt_status_text,
    rt_wants_ui, rt_ui_target, rt_ui_view, rt_draw_indices,
};

} // namespace
} // namespace rt

extern "C" __declspec(dllexport) const RtExports* rt_get_exports(uint32_t host_version) {
    return host_version == RT_API_VERSION ? &rt::exports : nullptr;
}
