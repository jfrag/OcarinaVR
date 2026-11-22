// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the ReTraced project
//
// The one interface between the ReTraced core (retraced.dll) and the adapter that lives inside the host renderer.
// Plain C, plain structs, versioned. No host type ever appears here: the core must build and run with any host that
// emits SPIR-V and Vulkan.
//
// Rules:
//  - The adapter loads retraced.dll beside the executable and asks for rt_get_exports(RT_API_VERSION). A null answer,
//    a missing DLL or RETRACED=0 means the host runs exactly as without us.
//  - Every entry is called serialised by the host (its replay lock), except spirv_patch / spirv_release, which run on
//    shader-translation worker threads and must be thread-safe.
//  - Strings returned by the core (extension names) stay valid for the life of the process.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define RT_API_VERSION 15u

#ifdef __cplusplus
extern "C" {
#endif

// A range of the game's memory as the host holds it on the GPU (its mirror of a memory pool).
typedef struct RtBuffer {
    VkBuffer buffer;
    uint64_t offset;
    VkDeviceAddress address;       // of offset
} RtBuffer;

// What the host offers the core (a table of functions, filled by the adapter).
typedef struct RtHost {
    uint32_t version;              // RT_API_VERSION
    const char* host_name;         // "longshot" (for the report only)
    const char* host_revision;     // pinned base revision, for the report
    void (*log)(const char* line); // into the host's own log, prefixed by the adapter
    // Submits on the host's queue under the host's own queue lock (one queue: the host's submits and presents take turns).
    int (*queue_submit)(const VkSubmitInfo2* submit, VkFence fence);
    // The GPU copy of a range of the game's memory (gpu_addr: the game's GPU address); 0 when the host has none.
    int (*mirror)(uint64_t gpu_addr, uint64_t size, RtBuffer* out);
    // The game's memory on the CPU at a GPU address (null if unmapped); *avail: bytes readable from there.
    const uint8_t* (*guest)(uint64_t gpu_addr, uint64_t* avail);
    // Runs fn(arg) under the host's queue lock: for work that uses the queue outside queue_submit (a headset's runtime
    // takes the queue when a frame begins and ends).
    void (*queue_locked)(void (*fn)(void* arg), void* arg);
} RtHost;

// One draw of the game, as the host's draw path resolved it (plain values; all addresses are the game's GPU addresses).
typedef struct RtDraw {
    uint64_t program_key;
    int32_t prim;                  // NVN primitive (4 = triangles)
    uint32_t indexed, index_type;  // index_type: NVN (0 byte, 1 short, 2 int)
    uint32_t mesh, indirect;
    int32_t count, first, base_vertex, instances;
    uint64_t index_addr;
    uint32_t attr0;                // NVN vertex attribute 0: format (bits 0-7), offset (8-26), stream (27-31)
    uint32_t int_attrs;            // how many vertex attributes have an integer format (bone indices: a skinned mesh)
    uint32_t stride0;              // stride of attribute 0's stream
    uint64_t vbuf_addr, vbuf_size; // attribute 0's stream
    uint64_t vs_c3, vs_c3_size;    // vertex stage uniform blocks c3 (gsys_context) and c4 (gsys_shape for most programs)
    uint64_t vs_c4, vs_c4_size;
    uint32_t n_colors;
    VkFormat color0_format;
    VkImage color0_image;
    VkImageUsageFlags color0_usage;
    VkImage depth_image;
    VkImageView depth_view;
    VkFormat depth_format;
    VkImageUsageFlags depth_usage;
    uint32_t width, height;        // of the targets
    uint32_t depth_write, depth_test;
} RtDraw;

// The Vulkan objects the host created.
typedef struct RtDevice {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
} RtDevice;

// End of one presented frame of the host.
typedef struct RtFrame {
    uint64_t number;               // presents since start, counted by the adapter
} RtFrame;

// The bytes of one uniform block of a watched program, as the host bound it for a draw.
typedef struct RtUniform {
    uint64_t program_key;
    int32_t stage;                 // 0 vertex, 1 fragment, 5 compute
    uint32_t bank;                 // constant bank (3 + NVN binding)
    const void* bytes;             // null when the host could not see them on the CPU
    uint32_t size;
    uint32_t source;               // 0 host's per-recording copy (exact), 1 guest memory at recording time, 2 unseen
    uint64_t guest_addr;           // the game's address of the block (0: unknown); the range bound starts there
    uint32_t ours;                 // bit 0: the draw's fragment module is one the core replaced with its per-pixel
                                   // outputs; bit 1: it also carries the switch on the sun's visibility
} RtUniform;

// One set of render targets of the game, as the host is about to use it (extra_targets).
typedef struct RtTargets {
    uint32_t n;                    // colour targets of the game
    VkImageView color0_view;
    VkFormat color0_format;
    uint32_t width, height;
    VkFormat depth_format;         // VK_FORMAT_UNDEFINED: no depth image
    uint32_t samples;
} RtTargets;

typedef struct RtExports {
    uint32_t version;              // RT_API_VERSION
    uint32_t size;                 // sizeof(RtExports), for growth

    // 1 = active, 0 = the core asks to be left out (disabled in retraced.ini, error): the adapter then stays inert.
    int (*init)(const RtHost* host);

    // Extensions the core wants, chosen among the available ones. Writes at most cap names, returns how many.
    uint32_t (*instance_extensions)(const char* const* available, uint32_t n_available, const char** out, uint32_t cap);
    uint32_t (*device_extensions)(const char* const* available, uint32_t n_available, const char** out, uint32_t cap);

    // Feature structures to query and enable: *tail points at the pNext field of the host's last structure; the core
    // links its own structures (static storage) and moves *tail to the last pNext it added.
    void (*device_features)(void*** tail);

    void (*device_created)(const RtDevice* device);

    // Every SPIR-V module the host creates passes here before vkCreateShaderModule. Returns non-zero and sets *out /
    // *out_words when the core replaces the code (the adapter then calls spirv_release(*out) after creation); 0 keeps
    // the original. Bit 1 of the result: the replacement is a fragment module that also writes our per-pixel outputs
    // (locations 1 to 4): the host must let it write the targets extra_targets added. Bit 2: it also carries the
    // switch on the sun's visibility (RtUniform.ours bit 1 for its draws).
    int (*spirv_patch)(const uint32_t* words, size_t n_words, const uint32_t** out, size_t* out_words);
    void (*spirv_release)(const uint32_t* out);

    void (*frame)(const RtFrame* frame);

    // Program keys whose uniform blocks the core wants to see (asked once at init; at most cap). The host then calls
    // uniform_seen for each uniform block it binds for those programs only (may be on several recording threads).
    // Two keys stand for many programs: ~0, every block of every program; ~1, the fragment stage's banks 6 and 7 of the
    // programs whose fragment module is ours (what the light by rays changes).
    uint32_t (*watched_programs)(uint64_t* keys, uint32_t cap);
    // Returns 0, or the number of bytes (= u->size) of a changed copy written to out, which the host binds instead.
    uint32_t (*uniform_seen)(const RtUniform* u, void* out, uint32_t cap);

    // The window's swapchain format, chosen among the surface's: returns 1 and writes *pick to replace the host's choice.
    int (*swapchain_format)(const VkSurfaceFormatKHR* formats, uint32_t n, VkSurfaceFormatKHR* pick);
    // The window's swapchain was (re)made with this format.
    void (*swapchain_created)(VkSwapchainKHR swapchain, VkSurfaceFormatKHR format);
    // Program keys whose draws the host must wait for rather than leave out while they are prepared (end of frame).
    uint32_t (*must_draw_programs)(uint64_t* keys, uint32_t cap);
    // The host's overlays (its menu) are drawn on the CPU as 8-bit B, G, R, A bytes and copied as they are into the
    // window's image: with our HDR10 window the core rewrites them in place as A2B10G10R10 words, PQ-encoded at the
    // display's paper white. Does nothing for an SDR window.
    void (*overlay_pixels)(uint8_t* bgra, size_t n);
    // 1 when the core wants every draw (draw is then called for each); read once at device creation.
    int (*wants_draws)(void);
    // Returns flags. Bit 0: the core wants to record its own commands right now, before this draw: the host then ends
    // its render pass, calls inject with its command buffer, and forgets its cached state. Bit 1: the host calls
    // xfb_begin right before this draw's own draw command and xfb_end right after it (inside the render pass).
    // Bit 2: see draw_indices. Bit 3: the core wants to see (and may replace) this draw's uniform blocks. Bit 4: this
    // draw writes its depth whatever the game set (hook Z1; the VR mode, for a translucent figure).
    int (*draw)(const RtDraw* draw);
    void (*inject)(VkCommandBuffer cmd);
    // A recording of the host begins (several per frame): the previous frame's commands are all submitted.
    void (*recording)(void);
    void (*xfb_begin)(VkCommandBuffer cmd);
    void (*xfb_end)(VkCommandBuffer cmd);
    // 1 when the core may add colour targets of its own (read once at device creation). extra_targets is then called
    // each time the host works out a set of targets: it writes up to 4 views and formats, to be attached AFTER the
    // game's own, and returns how many. A draw writes them only if its fragment module came back from spirv_patch
    // with bit 1; every other draw's write mask for them must be zero.
    int (*wants_targets)(void);
    uint32_t (*extra_targets)(const RtTargets* targets, VkImageView* views, VkFormat* formats);

    // The rows ReTraced adds to Longshot's Insert menu (Graphics tab), changed while the game runs: id 0 clouds in
    // volume (0/1), 1 share of the sky they cover (0..100), 2 water mirror (0/1), 3 light shafts (0/1), 4 the status
    // line under the FPS counter (0/1). live_set also keeps the choice for the next start (retraced_menu.ini).
    int (*live_get)(int id);
    void (*live_set)(int id, int value);
    // The status line's text (what is on, what it costs), for the FPS counter's corner; never null.
    const char* (*status_text)(void);

    // VR mode: the game's interface on an image of the core's. wants_ui: 1 when the core wants to see every set with
    // one colour target the host works out (read once at device creation). ui_target: such a set (image, its format
    // and usage, the size, whether a depth image comes with it); the core may give a view of its own image of the
    // same size, and its format, to be drawn into in its place: it then returns 1. ui_view: 1 when a view is that
    // image's: its draws must write alpha as coverage (see the adapter).
    int (*wants_ui)(void);
    int (*ui_target)(VkImage image, VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, int has_depth,
                     VkImageView* view, VkFormat* view_format);
    int (*ui_view)(VkImageView view);

    // VR mode, first person: after draw() returned bit 2 for an indexed draw, the indices to draw in place of the
    // draw's own (32-bit, the same base vertex). A count of 0: nothing of this draw is drawn.
    const uint32_t* (*draw_indices)(uint32_t* count);
} RtExports;

typedef const RtExports* (*PFN_rt_get_exports)(uint32_t host_version);

#ifdef RT_BUILDING_CORE
__declspec(dllexport) const RtExports* rt_get_exports(uint32_t host_version);
#endif

#ifdef __cplusplus
}
#endif
