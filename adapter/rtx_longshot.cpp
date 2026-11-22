// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the ReTraced project
//
// ReTraced adapter for Longshot (the continuation of Hookshot): the only file of ours that sees Longshot's internals.
// tools/apply-hooks.ps1 copies it into gpu/vk/ of Longshot's source (with rt_api.h written in place of its #include, so
// that Longshot's build sees every change), and adds the few call lines listed in hooks.json. It is compiled into the
// renderer, longshot_vk.dll; it loads retraced.dll beside the executable and turns Longshot's events into the calls of
// rt_api.h. Without the DLL, or with RETRACED=0, every hook returns at once: stock Longshot.
#include "vk_internal.h"
#include "vk_loader.h"
#include "vk_state.h"
#include "vk_draw.h"
#include "menu_module.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "rt_api.h"   // (replaced by the file itself when copied)

#ifndef RTX_HOST_REVISION
#define RTX_HOST_REVISION "?"
#endif

namespace {

const RtExports* g_rt = nullptr;
bool g_tried = false;
uint64_t g_presents = 0;
PFN_vkCreateShaderModule g_real_create_module = nullptr;
uint64_t g_watch[16];
uint32_t g_nwatch = 0;
uint64_t g_must[16];
uint32_t g_nmust = 0;

void host_log(const char* line) { mkwvk::logf("retraced: %s", line); }

// (the queue is shared with Longshot's submits and presents, which take dev.present_mu: "one queue, they take turns";
// the submits Longshot holds back (RX_VK_BATCH) go first, so the queue keeps their order)
int host_queue_submit(const VkSubmitInfo2* si, VkFence fence) {
    std::lock_guard lk(mkwvk::dev.present_mu);
    mkwvk::queue_flush_locked();
    return vkx::vkQueueSubmit2(mkwvk::dev.queue, 1, si, fence) == VK_SUCCESS;
}

int host_mirror(uint64_t gpu, uint64_t size, RtBuffer* out) {
    const mkwvk::BufferRef b = mkwvk::gpu_buffer(gpu, size);
    if (!b) return 0;
    out->buffer = b.buffer;
    out->offset = b.offset;
    out->address = b.address;
    return 1;
}

const uint8_t* host_guest(uint64_t gpu, uint64_t* avail) {
    return (mkwvk::host && mkwvk::host->gpu_ptr) ? mkwvk::host->gpu_ptr(gpu, avail) : nullptr;
}

void host_queue_locked(void (*fn)(void*), void* arg) {
    std::lock_guard lk(mkwvk::dev.present_mu);
    fn(arg);
}

const RtExports* core() {
    if (g_tried) return g_rt;
    g_tried = true;
    if (const char* e = std::getenv("RETRACED"); e && std::strcmp(e, "0") == 0) {
        mkwvk::logf("retraced: off (RETRACED=0)");
        return nullptr;
    }
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    path[n] = 0;
    wcscat(path, L"retraced.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        mkwvk::logf("retraced: no retraced.dll beside the executable, Longshot runs alone");
        return nullptr;
    }
    auto get = (PFN_rt_get_exports)(void*)GetProcAddress(m, "rt_get_exports");
    const RtExports* x = get ? get(RT_API_VERSION) : nullptr;
    if (!x || x->version != RT_API_VERSION || x->size < sizeof(RtExports)) {
        mkwvk::logf("retraced: retraced.dll does not speak interface %u, left out", RT_API_VERSION);
        return nullptr;
    }
    static const RtHost host = {RT_API_VERSION, "longshot", RTX_HOST_REVISION, host_log, host_queue_submit, host_mirror,
                                host_guest, host_queue_locked};
    if (!x->init(&host)) return nullptr;
    g_rt = x;
    return g_rt;
}

void add_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all,
                    uint32_t (*pick)(const char* const*, uint32_t, const char**, uint32_t)) {
    if (!pick) return;
    std::vector<const char*> names;
    names.reserve(all.size());
    for (auto& e : all) names.push_back(e.extensionName);
    const char* out[64];
    uint32_t k = pick(names.data(), (uint32_t)names.size(), out, 64);
    for (uint32_t i = 0; i < k && i < 64; ++i) {
        bool have = false;
        for (auto* w : want) have = have || std::strcmp(w, out[i]) == 0;
        if (!have) want.push_back(out[i]);
    }
}

std::shared_mutex g_gbuf_mu;
std::unordered_set<VkShaderModule> g_gbuf_modules, g_sun_modules;

VkResult VKAPI_CALL create_module(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a,
                                  VkShaderModule* m) {
    const uint32_t* out = nullptr;
    size_t words = 0;
    if (const int kind = g_rt && ci && ci->pCode ? g_rt->spirv_patch(ci->pCode, ci->codeSize / 4, &out, &words) : 0) {
        VkShaderModuleCreateInfo c = *ci;
        c.pCode = out;
        c.codeSize = words * 4;
        VkResult r = g_real_create_module(d, &c, a, m);
        g_rt->spirv_release(out);
        if (r == VK_SUCCESS && (kind & 2) && m) {   // (a fragment module with our per-pixel outputs: T2, T3 ask)
            std::unique_lock lk(g_gbuf_mu);
            g_gbuf_modules.insert(*m);
            if (kind & 4) g_sun_modules.insert(*m);
        }
        return r;
    }
    return g_real_create_module(d, ci, a, m);
}

} // namespace

namespace mkwvk {

bool rtx_watch_on = false;   // (read by H12 before the call: a draw of a program nobody watches costs one test)
bool rtx_must_on = false;    // (read by W1/W2: only when a draw is not ready yet)
bool rtx_draw_on = false;    // (read by H10 before the call: one test per draw when off)
bool rtx_gbuf_on = false;    // (read by T1 to T4: the core adds colour targets of its own)
bool rtx_ui_on = false;      // (read by V6: the VR mode draws the game's interface into an image of the core's)

// W1/W2: the end-of-frame programs the core names are waited for, never left out while they are prepared (a frame without
// them shows an older picture: the flicker of the first HDR start)
bool rtx_must_draw(uint64_t key) {
    for (uint32_t i = 0; i < g_nmust; ++i)
        if (g_must[i] == key) return true;
    return false;
}

void rtx_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (auto* x = core()) add_extensions(want, all, x->instance_extensions);
}

void rtx_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (auto* x = core()) add_extensions(want, all, x->device_extensions);
}

void rtx_device_features(void**& tail) {
    if (auto* x = core(); x && x->device_features) {
        void** t = tail;
        x->device_features(&t);
        tail = t;
    }
}

void rtx_device_created() {
    auto* x = core();
    if (!x) return;
    // every SPIR-V module of the game and of Longshot goes through vkx::vkCreateShaderModule (vk_loader.h); wrapping the
    // pointer here keeps vk_shader.cpp, which is in the program cache's stamp, untouched
    g_real_create_module = vkx::vkCreateShaderModule;
    vkx::vkCreateShaderModule = create_module;
    RtDevice d{};
    d.instance = dev.instance;
    d.physical = dev.physical;
    d.device = dev.device;
    d.queue = dev.queue;
    d.queue_family = dev.queue_family;
    d.get_instance_proc = vkx::instance_proc_addr();
    d.get_device_proc = vkx::vkGetDeviceProcAddr;
    x->device_created(&d);
    g_nwatch = x->watched_programs ? x->watched_programs(g_watch, 16) : 0;
    rtx_watch_on = g_nwatch > 0;
    g_nmust = x->must_draw_programs ? x->must_draw_programs(g_must, 16) : 0;
    rtx_must_on = g_nmust > 0;
    rtx_draw_on = x->wants_draws && x->wants_draws();
    rtx_gbuf_on = x->wants_targets && x->extra_targets && x->wants_targets();
    rtx_ui_on = x->wants_ui && x->ui_target && x->ui_view && x->wants_ui();
}


// (VR mode, first person: the indices the core gives for the draw being made, in place of its own. 0: its own; -1: the
// draw is left out. Set by H10's call, read by H10 and B1 on the same thread.)
thread_local int rtx_alt_n = 0;
thread_local const uint32_t* rtx_alt_idx = nullptr;
// (the core asked to see the uniform blocks of the draw being made, whatever its program: H12 reads it)
thread_local bool rtx_alt_blocks = false;
thread_local uint64_t rtx_rec_serial = 0;   // (hook Y1) counts this thread's command recordings: a changed block is kept for one
thread_local bool rtx_dw_now = false;   // (hook Z1) this draw writes its depth whatever the game set (the VR mode: a translucent figure)

// H10, in draw() once the pipeline is known: the draw's plain values for the core
int rtx_draw(const NvnState& s, const DrawArgs& d, uint64_t key, int n_colors, VkFormat color0, VkImage color0_image,
              VkImageUsageFlags color0_usage, VkImage depth_image, VkImageView depth_view, VkFormat depth_format,
              VkImageUsageFlags depth_usage, uint32_t w, uint32_t h) {
    if (!g_rt) return 0;
    RtDraw r{};
    r.program_key = key;
    r.prim = d.prim, r.indexed = d.indexed, r.index_type = (uint32_t)d.itype, r.mesh = d.mesh, r.indirect = d.indirect != 0;
    r.count = d.count, r.first = d.first, r.base_vertex = d.base_vertex, r.instances = d.instances;
    r.index_addr = d.ibuf;
    r.attr0 = s.nvattr > 0 ? s.vattr[0] : 0;
    // (NVN's integer vertex formats, R8UI ... RGBA32I: 0x3 0x4 0x8 0x9 0xB 0xC 0xF 0x10 0x14 0x15 0x17 0x18 0x1B 0x1C 0x20 0x21
    // 0x23 0x24 0x27 0x28 0x2C 0x2D 0x2F 0x30)
    constexpr uint64_t int_formats = 0x0001B19B19B19B18ull;
    for (int i = 0; i < s.nvattr && i < 16; ++i) {
        const uint32_t f = s.vattr[i] & 0xff;
        if (f < 64 && ((int_formats >> f) & 1)) r.int_attrs++;
    }
    const uint32_t stream = (r.attr0 >> 27) & 31;
    r.stride0 = s.vstream[stream].stride;
    r.vbuf_addr = s.vbuf[stream].address, r.vbuf_size = s.vbuf[stream].size;
    r.vs_c3 = s.ubo[0][0].addr, r.vs_c3_size = s.ubo[0][0].size;
    r.vs_c4 = s.ubo[0][1].addr, r.vs_c4_size = s.ubo[0][1].size;
    r.n_colors = (uint32_t)n_colors, r.color0_format = color0, r.color0_image = color0_image, r.color0_usage = color0_usage;
    r.depth_image = depth_image, r.depth_view = depth_view, r.depth_format = depth_format, r.depth_usage = depth_usage;
    r.width = w, r.height = h;
    r.depth_write = s.ds.dw, r.depth_test = s.ds.dt;
    rtx_alt_n = 0;
    const int rf = g_rt->draw(&r);
    rtx_alt_blocks = (rf & 8) != 0;
    rtx_dw_now = (rf & 16) != 0;
    if ((rf & 4) && g_rt->draw_indices) {   // (the core gives this draw's indices: B1 draws them; none: H10 leaves the draw out)
        uint32_t n = 0;
        rtx_alt_idx = g_rt->draw_indices(&n);
        rtx_alt_n = n && rtx_alt_idx ? (int)n : -1;
    }
    return rf;
}

thread_local bool rtx_xfb_now = false;   // (set by H10 for the draw being made, read around its draw command)

// XA, XC, right around the draw command, inside the render pass: the capture of this draw's positions
void rtx_xfb_begin(VkCommandBuffer cmd) {
    if (g_rt && g_rt->xfb_begin) g_rt->xfb_begin(cmd);
}
void rtx_xfb_end(VkCommandBuffer cmd) {
    if (g_rt && g_rt->xfb_end) g_rt->xfb_end(cmd);
}

// H10's second half: the core records its own commands into the frame's command buffer (Longshot's pass is ended)
void rtx_inject(VkCommandBuffer cmd) {
    if (g_rt && g_rt->inject) g_rt->inject(cmd);
}

// H13, at the start of each recording: the previous frame's commands are all submitted
void rtx_recording() {
    if (g_rt && rtx_draw_on) g_rt->recording();
}

// H12, in build_descriptors: a uniform block bound for a draw or dispatch. Only programs the core watches go further.
// T1, at the end of the host's targets(): our views after the game's (no host texture behind them)
void rtx_targets(int& n, VkImageView* views, VkFormat* formats, uint32_t width, uint32_t height, VkFormat depth_format,
                 VkSampleCountFlagBits samples) {
    if (!g_rt || n < 1 || n > 4) return;
    RtTargets t{};
    t.n = (uint32_t)n, t.color0_view = views[0], t.color0_format = formats[0];
    t.width = width, t.height = height, t.depth_format = depth_format, t.samples = (uint32_t)samples;
    VkImageView v[4]{};
    VkFormat f[4]{};
    const uint32_t k = std::min<uint32_t>(g_rt->extra_targets(&t, v, f), 4);
    for (uint32_t i = 0; i < k && n < 8; ++i) views[n] = v[i], formats[n] = f[i], ++n;
}

// V6, at the end of the host's targets(): a set with one colour target. The core may put a view of its own in its place
// (the game's interface, drawn into the finished picture, goes to an image of the core's instead).
void rtx_ui_targets(VkImage image, VkImageUsageFlags usage, VkImageView& view, VkFormat& format, uint32_t w, uint32_t h, bool depth) {
    if (!g_rt) return;
    VkImageView v = VK_NULL_HANDLE;
    VkFormat f = VK_FORMAT_UNDEFINED;
    if (g_rt->ui_target(image, format, usage, w, h, depth ? 1 : 0, &v, &f) && v) view = v, format = f;
}

// V6, in the dynamic state: draws into the core's interface image. The game blends its interface into a picture that
// has no alpha; into ours, alpha must come out as coverage (what the picture behind keeps is 1 - alpha): an alpha-blended
// draw adds its own alpha over what is there, an additive draw (a glow) leaves it. Colour is blended as the game asks.
void rtx_ui_blend(VkImageView view, VkBool32 en, VkColorBlendEquationEXT& eq, VkColorComponentFlags& wm) {
    if (!g_rt || !g_rt->ui_view(view)) return;
    if (wm) wm |= VK_COLOR_COMPONENT_A_BIT;
    if (!en) return;
    if (eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA)
        eq.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, eq.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, eq.alphaBlendOp = VK_BLEND_OP_ADD;
    else if (eq.dstColorBlendFactor == VK_BLEND_FACTOR_ONE)
        eq.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, eq.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, eq.alphaBlendOp = VK_BLEND_OP_ADD;
}

// T2, T3: is this fragment module one of ours? 0 no; 1 with the per-pixel outputs; 3 with the sun switch too
int rtx_gbuf_module(VkShaderModule m) {
    std::shared_lock lk(g_gbuf_mu);
    return g_gbuf_modules.count(m) ? (g_sun_modules.count(m) ? 3 : 1) : 0;
}

static std::atomic<uint64_t> g_kept_hits{0}, g_kept_made{0};   // (changed blocks given again / copied into the stream)
uint64_t rtx_kept_blocks(int made) { return (made ? g_kept_made : g_kept_hits).load(std::memory_order_relaxed); }

void rtx_uniform(const NvnState& s, uint64_t key, int ns, uint32_t bank, VkDescriptorBufferInfo& bi, VkBuffer stream,
                 const uint8_t* mapped, bool (*substitute)(const void*, uint32_t, VkDescriptorBufferInfo*)) {
    const uint32_t ours = ((uint32_t)ns >> 8) & 3;   // (H12: bit 8 the program's fragment module is ours, bit 9 it has the sun switch)
    ns &= 0xff;
    // (a watched key of ~0: every block of every program, one call each; of ~1: only the blocks the light by rays
    // changes, the fragment stage's banks 6 and 7 of the programs whose fragment module is ours. With ~0 the light by
    // rays cost 8 frames a second in the forest: some 27 000 calls a frame, most of them for nothing.)
    bool hit = false;
    for (uint32_t i = 0; i < g_nwatch; ++i)
        hit = hit || g_watch[i] == key || g_watch[i] == ~0ull || (g_watch[i] == ~1ull && ns == 1 && (bank == 6 || bank == 7) && (ours & 1));
    hit = hit || rtx_alt_blocks;
    if (!hit || !g_rt) return;
    RtUniform u{};
    u.program_key = key;
    u.stage = ns;
    u.ours = ours;
    u.bank = bank;
    // (observed and replaced up to 64 KB, the most a uniform range holds; programs that index a block dynamically are
    // given the whole range, so the copy is that big)
    u.size = bi.range == VK_WHOLE_SIZE || bi.range > 65536 ? 0 : (uint32_t)bi.range;
    u.source = 2;
    if (bank >= 3 && bank < 19) u.guest_addr = s.ubo[std::clamp(ns, 0, kNvnStages - 1)][bank - 3].addr;
    if (bi.buffer && bi.buffer == stream && mapped) {   // (Longshot's copy for this recording, inline updates in: exact)
        u.bytes = mapped + bi.offset;
        u.source = 0;
    } else if (bank >= 3 && bank < 19) {   // (a pool's mirror: the guest's memory as it is now, maybe a frame ahead)
        const auto& b = s.ubo[std::clamp(ns, 0, kNvnStages - 1)][bank - 3];
        uint64_t avail = 0;
        const uint8_t* p = (host && host->gpu_ptr && b.addr) ? host->gpu_ptr(b.addr, &avail) : nullptr;
        if (p && avail >= u.size) u.bytes = p, u.source = 1;
    }
    // the core may hand back a changed copy (same size): written into this recording's stream, bound in place
    alignas(16) static thread_local uint8_t copy[65536];
    const uint32_t n = g_rt->uniform_seen(&u, copy, sizeof copy);
    if (!(n && n == u.size && bi.range != VK_WHOLE_SIZE && bi.range <= sizeof copy && substitute)) return;
    // The same block changed the same way is asked for by hundreds of draws of a recording (the environment block of
    // every lit program, with the light by rays on; Link's pose for each of his pieces). At first each draw
    // got its own copy in the stream, so each draw had another place to bind and the host pushed its descriptors again:
    // measured in the VR mode, the light by rays alone cost 8 frames a second in the forest, nearly all of it on the
    // processor. Now a copy made in this recording is given again as long as its bytes are the same: the host sees
    // the same place and pushes nothing. (rtx_kept_blocks = 0: as before.)
    struct Kept {
        uint64_t serial = 0, addr = 0, used = 0;
        VkDescriptorBufferInfo info{};
        std::vector<uint8_t> bytes;
    };
    static thread_local Kept kept[24];
    static thread_local uint64_t tick = 0;
    static const bool on = !getenv("RETRACED_KEPT_BLOCKS") || atoi(getenv("RETRACED_KEPT_BLOCKS")) != 0;
    ++tick;
    if (on && rtx_rec_serial)
        for (Kept& k : kept)
            if (k.serial == rtx_rec_serial && k.addr == u.guest_addr && k.bytes.size() == n && !std::memcmp(k.bytes.data(), copy, n)) {
                bi = k.info, k.used = tick;
                g_kept_hits.fetch_add(1, std::memory_order_relaxed);
                return;
            }
    VkDescriptorBufferInfo o{};
    if (!substitute(copy, n, &o)) return;
    bi = o;
    g_kept_made.fetch_add(1, std::memory_order_relaxed);
    if (on && rtx_rec_serial) {
        Kept* k = &kept[0];   // (the one of another recording, or else the one not asked for the longest)
        for (Kept& c : kept)
            if ((c.serial != rtx_rec_serial ? 0 : c.used) < (k->serial != rtx_rec_serial ? 0 : k->used)) k = &c;
        k->serial = rtx_rec_serial, k->addr = u.guest_addr, k->info = o, k->used = tick;
        k->bytes.assign(copy, copy + n);
    }
}

// H5, in the swapchain's making: the core may pick another surface format (HDR10)
void rtx_swapchain_format(const std::vector<VkSurfaceFormatKHR>& formats, VkSurfaceFormatKHR& fmt) {
    if (!g_rt || !g_rt->swapchain_format) return;
    VkSurfaceFormatKHR pick = fmt;
    if (g_rt->swapchain_format(formats.data(), (uint32_t)formats.size(), &pick)) fmt = pick;
}

// H6, once the swapchain is made: HDR metadata
void rtx_swapchain_created(VkSwapchainKHR sc, const VkSurfaceFormatKHR& fmt) {
    if (g_rt && g_rt->swapchain_created) g_rt->swapchain_created(sc, fmt);
}

// The rows ReTraced adds to the Insert menu (the OcarinaVR module below): the menu writes these values, each frame the
// ones that changed go to the core.
constexpr int kRtxMenuRows = 21;   // (20: Link's arms on the player's hands)     // (18, 19: the hands' tilt, the player's arm)     // (16, 17: the VR mode's whole body, gestures)   // (13 to 15: the VR mode's first person, big screen, turn by steps)   // (clouds, cover, water, shafts, status line, light by rays, its share, its sky, sun on the water, caustics, simulated water, its waves, water in particles)
int rtx_menu_val[21] = {0, 45, 0, 0, 1, 0, 100, 100, 1, 25, 1, 100, 0, 1, 0, 1, 0, 1, 0, 55, 1};
int rtx_menu_sent[21] = {};
bool rtx_menu_ready = false;
bool rtx_has_live() { return g_rt && g_rt->size >= sizeof(RtExports) && g_rt->live_get && g_rt->live_set && g_rt->status_text; }
void rtx_menu_poll() {
    if (!rtx_has_live()) return;
    if (!rtx_menu_ready) {
        for (int i = 0; i < kRtxMenuRows; ++i) rtx_menu_val[i] = rtx_menu_sent[i] = g_rt->live_get(i);
        rtx_menu_ready = true;
        return;
    }
    for (int i = 0; i < kRtxMenuRows; ++i) {
        if (rtx_menu_val[i] != rtx_menu_sent[i]) rtx_menu_sent[i] = rtx_menu_val[i], g_rt->live_set(i, rtx_menu_val[i]);
        else if (i >= 13) {   // (the VR rows are also changed by keys and by the headset's controllers)
            const int now = g_rt->live_get(i);
            if (now != rtx_menu_sent[i]) rtx_menu_val[i] = rtx_menu_sent[i] = now;
        }
    }
}

// The VR mode's rows, first on the Graphics tab under their own heading (Longshot's menu modules, gpu/vk/menu_module.h).
// Their values are the core's: read from it at the first frame, sent to it when they change (rtx_menu_poll).
namespace {
menu::Setting rtx_onoff_row(const char* key, const char* label, int i, const char* help) {
    return {.key = key, .label = label, .help = help, .kind = menu::Kind::Toggle, .value = &rtx_menu_val[i], .lo = 0, .hi = 1,
            .preset_exempt = true};
}
menu::Setting rtx_number_row(const char* key, const char* label, int i, int lo, int hi, int step, const char* help) {
    return {.key = key, .label = label, .help = help, .kind = menu::Kind::Slider, .value = &rtx_menu_val[i], .lo = lo, .hi = hi,
            .step = step, .preset_exempt = true, .text = [](int v) { return std::to_string(v); }};
}
} // namespace

MENU_MODULE(ocarinavr) {
    menu::tab("graphics")
        .heading("ocarinavr", "OcarinaVR", menu::first())
        .row(rtx_onoff_row("ocarinavr_first_person", "First person", 13,
                           "Headset: the view from Link's eyes (off: the game's camera, behind him). Key P. At once."))
        .row(rtx_onoff_row("ocarinavr_big_screen", "Big screen", 14,
                           "Headset: the game on a big screen in relief, with its own camera (off: inside the game). Key V. At once."))
        .row(rtx_onoff_row("ocarinavr_snap_turning", "Snap turning", 15,
                           "Headset, first person: the right stick turns in 30-degree steps (easier on the stomach); off: smoothly. At once."))
        .row(rtx_onoff_row("ocarinavr_body_visible", "Link's body visible", 16,
                           "Headset, first person: Link's whole body under your eyes (off: his arms alone, his hands exactly on yours). At once."))
        .row(rtx_onoff_row("ocarinavr_gestures", "Gestures", 17,
                           "Headset: strike with a real swing, draw from your back, raise the shield, take and throw, aim with both hands. Off: buttons only. At once."))
        .row(rtx_onoff_row("ocarinavr_hands", "Link's hands on yours", 20,
                           "Headset: Link's arms follow your controllers or bare hands, fingers included (off: his arms keep the game's pose). At once."))
        .row(rtx_number_row("ocarinavr_hand_tilt", "Hand tilt", 18, -90, 90, 5,
                            "Headset: if Link's hands are tilted against yours, turn them forward or back (degrees). At once."))
        .row(rtx_number_row("ocarinavr_arm_length", "Your arm length", 19, 30, 80, 2,
                            "Headset: from your shoulder to your wrist, in centimetres; the world is scaled so that Link's arms are yours. At once."));
}

void rtx_frame() {
    if (!g_rt) return;
    RtFrame f{};
    f.number = ++g_presents;
    g_rt->frame(&f);
    rtx_menu_poll();
}

} // namespace mkwvk
