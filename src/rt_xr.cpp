// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the ReTraced project
#include "rt_xr.h"

#include "rt_diag.h"
#include "rt_hands.h"
#include "rt_rec.h"
#include "rt_vk.h"
#include "rt_vr.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt::xr {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- the loader's functions, all through xrGetInstanceProcAddr (nothing is linked) ----
#define RT_XR_FUNCS(X)                                                                                                \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) X(xrResultToString)       \
    X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession) X(xrCreateReferenceSpace)             \
    X(xrDestroySpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain)                       \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)       \
    X(xrPollEvent) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews)    \
    X(xrLocateSpace) X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings)  \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat)                 \
    X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) X(xrGetVulkanInstanceExtensionsKHR)                          \
    X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) X(xrGetVulkanGraphicsRequirementsKHR)          \
    X(xrCreateActionSpace) X(xrGetActionStatePose)
#define RT_XR_DECLARE(f) PFN_##f f = nullptr;
RT_XR_FUNCS(RT_XR_DECLARE)
#undef RT_XR_DECLARE
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
PFN_xrCreateInstance xrCreateInstance = nullptr;
PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB = nullptr;
PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB = nullptr;
PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB = nullptr;
PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT = nullptr;
PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT = nullptr;

void loop();   // (the headset's thread, below)
const RtHost* H = nullptr;
RtDevice g_dev{};
XrInstance instance = XR_NULL_HANDLE;
XrSystemId system_id = XR_NULL_SYSTEM_ID;
XrSession session = XR_NULL_HANDLE;
XrSpace space_base = XR_NULL_HANDLE, space_view = XR_NULL_HANDLE, space_app = XR_NULL_HANDLE;
std::string runtime_name, system_name, last_error = "", vk_inst_ext, vk_dev_ext;
std::vector<std::string> inst_names, dev_names;   // (kept: the names given to the host must stay valid)
uint32_t rec_w = 0, rec_h = 0;
bool have_cylinder = false, have_refresh = false, have_hands = false, have_hand_source = false, have_hand_aim = false;
bool g_boot_ok = false, running = false, frame_begun = false, should_render = false, centred = false, focused = false;
XrSessionState state = XR_SESSION_STATE_UNKNOWN;
XrTime frame_time = 0;
XrDuration frame_period = 0;
float refresh_hz = 0;
std::mutex space_mu;     // (the centred space and the frame's time: the game's thread asks for the head too)
int64_t frame_local = 0; // our clock when frame_time was given
double predict_ms = 30;  // how far past the headset's own frame time the head is asked for the game (the game's frame is seen later)
uint64_t st_copy_failed = 0, st_deferred = 0, st_late = 0, st_located = 0, st_chains_lost = 0;
int64_t last_turn = 0;
bool ever_focused = false;
std::atomic<const char*> closing{nullptr};   // (set when the session ends for good: why; read by the headset's thread)
std::atomic<bool> thread_on{false}, torn_down{false};
std::atomic<bool> runtime_loaded{false};   // a headset's runtime was loaded in this process (see exit_hard)
uint64_t st_teardowns = 0;
bool shown_ok = false;   // (a picture was released into the chains at least once)
uint64_t st_copies = 0;
uint64_t st_frames = 0, st_no_render = 0, st_projection = 0, st_screen = 0, st_errors = 0, st_recentres = 0;
int64_t wait_ns_total = 0, wait_ns_max = 0;
uint64_t wait_n = 0;
XrFovf eye_fov[2] = {};
double eye_ipd = 0, eye_turn[2] = {};   // (the gap between the eyes; how far each eye is turned against the head, degrees)

// the pictures' way to the headset
struct Chain {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t w = 0, h = 0;
};
Chain chain[3];   // left eye, right eye, the interface's panel
int64_t chain_format = 0;
VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer cmd[2] = {};
VkFence fence[2] = {};
int slot = 0;

// the big screen and the interface's panel (metres, degrees)
double screen_dist = 3.0, screen_angle = 110.0, screen_y = 0.0, ui_dist = 2.0, ui_angle = 70.0, ui_y = -0.05;
bool screen_curved = true;
double panel_yaw = 0;   // (the panel follows the head's heading, lazily)
bool panel_set = false;

// the controllers as a gamepad
enum { PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LSTICK, PAD_RSTICK, PAD_LSHOULDER,
       PAD_RSHOULDER, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_LTRIGGER, PAD_RTRIGGER };   // (the host's positions)
XrActionSet action_set = XR_NULL_HANDLE;
XrAction a_btn[7] = {}, a_val[4] = {}, a_stick[2] = {}, a_haptic = XR_NULL_HANDLE;
// the hands themselves (Link's arms follow them): where each controller is held and where it points; and,
// when the headset follows bare hands, every joint of every finger
XrAction a_grip = XR_NULL_HANDLE, a_aim = XR_NULL_HANDLE;
XrSpace space_grip[2] = {}, space_aim[2] = {};
XrHandTrackerEXT tracker[2] = {};
bool hands_supported = false;
float pad_val[4] = {};           // (the triggers and the grips, 0 to 1: the fingers bend by them)
double hand_prev[2][3] = {};
int64_t hand_prev_t[2] = {};
uint64_t st_hand_ctrl = 0, st_hand_bare = 0;
XrPath hand_path[2] = {};
bool actions_ok = false;
std::mutex pad_mu;
// the game's vibration (hook V8), felt in the headset's controllers: strength 0 to 1 for each hand, and when it was asked
std::atomic<float> rumble_amp[2];
std::atomic<int64_t> rumble_time[2];
uint64_t st_rumbles = 0;
uint64_t pad_buttons = 0;
int32_t pad_stick[4] = {};
bool pad_active = false;
int64_t menu_down_at = 0, start_pulse_until = 0, lclick_pulse_until = 0;
bool person_chord_done = false, rec_chord_done = false;
std::atomic<int64_t> buzz_at{0};
std::atomic<int> buzz_kind{0};
// The video recorder asked to start or to end (a key, or the left stick held with B): the camera view comes on with
// it, and the controllers say which it was.
void rec_toggle() {
    if (!rec::available()) return;
    rec::toggle();
    if (rec::wanted()) vr::set_film(true);
    buzz_kind.store(rec::wanted() ? 2 : 1), buzz_at.store(now_ns());
}
bool menu_long_done = false, lclick_chorded = false, lclick_was = false, menu_was = false, mode_chord_done = false;

bool bad(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return false;
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (xrResultToString && instance) xrResultToString(instance, r, name);
    last_error = std::string(what) + ": " + name;
    if (st_errors++ < 40) log_host("xr: %s failed (%s)", what, name);
    return true;
}

template <class F> void locked(F&& f) {   // (the runtime takes the queue: under the host's queue lock)
    if (H && H->queue_locked) H->queue_locked([](void* p) { (*(F*)p)(); }, &f);
    else f();
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a < s.size()) {
        size_t z = s.find(' ', a);
        if (z == std::string::npos) z = s.size();
        if (z > a) v.push_back(s.substr(a, z - a));
        a = z + 1;
    }
    return v;
}

double opt_f(const char* key, double fallback) {
    const std::string v = opt(key, "");
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end && end != v.c_str() ? d : fallback;
}

void q_mul(const double a[4], const double b[4], double o[4]) {   // (x y z w)
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
void q_turn(const double q[4], const double v[3], double o[3]) {   // v turned by q
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; ++i) o[i] = r[i][0] * v[0] + r[i][1] * v[1] + r[i][2] * v[2];
}

XrPosef identity() {
    XrPosef p{};
    p.orientation.w = 1;
    return p;
}

bool make_app_space(const XrPosef& offset) {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = offset;
    XrSpace s = XR_NULL_HANDLE;
    if (bad(xrCreateReferenceSpace(session, &ci, &s), "xrCreateReferenceSpace (centred)")) return false;
    std::lock_guard lk(space_mu);
    if (space_app) xrDestroySpace(space_app);
    space_app = s;
    return true;
}

// The head as it is now becomes the origin: where the player sits and the way they face are "straight ahead, at the
// game camera's own place".
void recentre() {
    if (!session || !space_base || !space_view || !frame_time) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (bad(xrLocateSpace(space_view, space_base, frame_time, &loc), "xrLocateSpace (recentre)")) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    const XrQuaternionf& q = loc.pose.orientation;
    // the head's forward axis (-z) in the base space, laid flat: the heading
    const double fx = -2.0 * (q.x * q.z + q.w * q.y), fz = -(1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    const double yaw = std::atan2(-fx, -fz);
    XrPosef off = identity();
    off.orientation.y = (float)std::sin(yaw * 0.5), off.orientation.w = (float)std::cos(yaw * 0.5);
    off.position = loc.pose.position;
    if (make_app_space(off)) centred = true, panel_set = false, st_recentres++, log_host("xr: recentred (heading %.0f degrees)", yaw * 180.0 / kPi);
}

void make_actions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "jeu");
    std::strcpy(si.localizedActionSetName, "Jeu");
    if (bad(xrCreateActionSet(instance, &si, &action_set), "xrCreateActionSet")) return;
    xrStringToPath(instance, "/user/hand/left", &hand_path[0]);
    xrStringToPath(instance, "/user/hand/right", &hand_path[1]);
    auto make = [&](XrAction& a, const char* name, const char* shown, XrActionType type, bool hands) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        ci.actionType = type;
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, shown);
        if (hands) ci.countSubactionPaths = 2, ci.subactionPaths = hand_path;
        return !bad(xrCreateAction(action_set, &ci, &a), name);
    };
    static const char* const bn[7] = {"a", "b", "x", "y", "menu", "clic_gauche", "clic_droit"};
    static const char* const vn[4] = {"gachette_gauche", "gachette_droite", "poignee_gauche", "poignee_droite"};
    bool ok = true;
    for (int i = 0; i < 7; ++i) ok = ok && make(a_btn[i], bn[i], bn[i], XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    for (int i = 0; i < 4; ++i) ok = ok && make(a_val[i], vn[i], vn[i], XR_ACTION_TYPE_FLOAT_INPUT, false);
    ok = ok && make(a_stick[0], "stick_gauche", "stick gauche", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    ok = ok && make(a_stick[1], "stick_droit", "stick droit", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    ok = ok && make(a_haptic, "vibration", "vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    if (!ok) return;
    // (the hands' poses: if the runtime refuses them, the controllers stay a gamepad as before)
    bool poses = make(a_grip, "main_tenue", "main (tenue)", XR_ACTION_TYPE_POSE_INPUT, true) &&
                 make(a_aim, "main_visee", "main (visee)", XR_ACTION_TYPE_POSE_INPUT, true);
    if (!poses) a_grip = a_aim = XR_NULL_HANDLE, log_host("xr: the hands' pose actions were refused (%s): the controllers stay a gamepad", last_error.c_str());
    // the Touch controllers of a Quest (the profile every Quest answers to through Link)
    struct B { XrAction a; const char* path; };
    const B list[] = {
        {a_btn[0], "/user/hand/right/input/a/click"}, {a_btn[1], "/user/hand/right/input/b/click"},
        {a_btn[2], "/user/hand/left/input/x/click"}, {a_btn[3], "/user/hand/left/input/y/click"},
        {a_btn[4], "/user/hand/left/input/menu/click"}, {a_btn[5], "/user/hand/left/input/thumbstick/click"},
        {a_btn[6], "/user/hand/right/input/thumbstick/click"}, {a_val[0], "/user/hand/left/input/trigger/value"},
        {a_val[1], "/user/hand/right/input/trigger/value"}, {a_val[2], "/user/hand/left/input/squeeze/value"},
        {a_val[3], "/user/hand/right/input/squeeze/value"}, {a_stick[0], "/user/hand/left/input/thumbstick"},
        {a_stick[1], "/user/hand/right/input/thumbstick"}, {a_haptic, "/user/hand/left/output/haptic"},
        {a_haptic, "/user/hand/right/output/haptic"}, {a_grip, "/user/hand/left/input/grip/pose"},
        {a_grip, "/user/hand/right/input/grip/pose"}, {a_aim, "/user/hand/left/input/aim/pose"},
        {a_aim, "/user/hand/right/input/aim/pose"}};
    XrPath profile = XR_NULL_PATH;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile);
    auto suggest = [&](bool with_poses) {
        std::vector<XrActionSuggestedBinding> sb;
        for (const B& b : list) {
            if (!b.a || (!with_poses && (b.a == a_grip || b.a == a_aim))) continue;
            XrPath p = XR_NULL_PATH;
            xrStringToPath(instance, b.path, &p);
            sb.push_back({b.a, p});
        }
        XrInteractionProfileSuggestedBinding pb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        pb.interactionProfile = profile;
        pb.countSuggestedBindings = (uint32_t)sb.size();
        pb.suggestedBindings = sb.data();
        return !bad(xrSuggestInteractionProfileBindings(instance, &pb), "xrSuggestInteractionProfileBindings");
    };
    if (!poses || !suggest(true)) {
        if (poses) log_host("xr: the bindings with the hands' poses were refused (%s): tried again without them", last_error.c_str());
        poses = false;
        if (!suggest(false)) return;
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &action_set;
    if (bad(xrAttachSessionActionSets(session, &ai), "xrAttachSessionActionSets")) return;
    for (int h = 0; h < 2 && poses; ++h) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.subactionPath = hand_path[h];
        sc.poseInActionSpace = identity();
        sc.action = a_grip;
        if (XR_FAILED(xrCreateActionSpace(session, &sc, &space_grip[h]))) space_grip[h] = XR_NULL_HANDLE;
        sc.action = a_aim;
        if (XR_FAILED(xrCreateActionSpace(session, &sc, &space_aim[h]))) space_aim[h] = XR_NULL_HANDLE;
    }
    // bare hands: one tracker a hand (the headset follows them when the controllers are put down)
    if (have_hands && hands_supported && xrCreateHandTrackerEXT && xrLocateHandJointsEXT)
        for (int h = 0; h < 2; ++h) {
            XrHandTrackerCreateInfoEXT hc{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            hc.hand = h ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
            hc.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            XrHandTrackingDataSourceInfoEXT ds{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
            XrHandTrackingDataSourceEXT want_src[1] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
            ds.requestedDataSourceCount = 1, ds.requestedDataSources = want_src;
            if (have_hand_source) hc.next = &ds;   // (real hands only: a held controller's made-up fingers are not wanted)
            if (XR_FAILED(xrCreateHandTrackerEXT(session, &hc, &tracker[h]))) tracker[h] = XR_NULL_HANDLE;
        }
    log_host("xr: hands: controllers' poses %s; bare hands %s", space_grip[0] && space_grip[1] ? "ready" : "NOT available",
             tracker[0] && tracker[1] ? (have_hand_source ? "ready (real hands only)" : "ready") : have_hands ? "offered but not usable" : "not offered by the runtime");
    actions_ok = true;
}

// ---- the hands, once a headset frame (after the actions are read) ----
void q_to_m(const XrQuaternionf& q, double m[3][3]) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
}
void m_to_q(const double m[3][3], double q[4]) {   // (x y z w)
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s, q[0] = (m[2][1] - m[1][2]) / s, q[1] = (m[0][2] - m[2][0]) / s, q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s, q[0] = 0.25 * s, q[1] = (m[0][1] + m[1][0]) / s, q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s, q[0] = (m[0][1] + m[1][0]) / s, q[1] = 0.25 * s, q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s, q[0] = (m[0][2] + m[2][0]) / s, q[1] = (m[1][2] + m[2][1]) / s, q[2] = 0.25 * s;
    }
}

// How far a finger's joint is bent: the angle between the bone before it and the bone after it (a joint's -z runs
// along its bone to the fingertip, its +x across the hand), counted as bending when it folds towards the palm.
float bend(const XrHandJointLocationEXT& a, const XrHandJointLocationEXT& b) {
    double ma[3][3], mb[3][3];
    q_to_m(a.pose.orientation, ma), q_to_m(b.pose.orientation, mb);
    const double da[3] = {-ma[0][2], -ma[1][2], -ma[2][2]}, db[3] = {-mb[0][2], -mb[1][2], -mb[2][2]};
    const double c = std::clamp(da[0] * db[0] + da[1] * db[1] + da[2] * db[2], -1.0, 1.0);
    const double cr[3] = {da[1] * db[2] - da[2] * db[1], da[2] * db[0] - da[0] * db[2], da[0] * db[1] - da[1] * db[0]};
    const double side = cr[0] * ma[0][0] + cr[1] * ma[1][0] + cr[2] * ma[2][0];   // (along the joint's +x: unbending)
    return (float)(side <= 0 ? std::acos(c) : -std::min(std::acos(c), 0.3));
}

void read_hands() {
    if (!session || !space_app) return;
    XrTime t = 0;
    {
        std::lock_guard lk(space_mu);
        t = frame_time + (XrTime)(predict_ms * 1e6);   // (the game's picture of these hands is seen a little later)
    }
    const int64_t now = now_ns();
    for (int h = 0; h < 2; ++h) {
        hands::Hand out;
        // the bare hand first: when the headset follows it, its fingers are the real ones
        if (tracker[h] && focused) {
            XrHandJointLocationEXT j[XR_HAND_JOINT_COUNT_EXT];
            XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locs.jointCount = XR_HAND_JOINT_COUNT_EXT, locs.jointLocations = j;
            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            li.baseSpace = space_app, li.time = t;
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (XR_SUCCEEDED(xrLocateHandJointsEXT(tracker[h], &li, &locs)) && locs.isActive &&
                (j[XR_HAND_JOINT_PALM_EXT].locationFlags & need) == need) {
                const XrPosef& palm = j[XR_HAND_JOINT_PALM_EXT].pose;
                double m[3][3], g[3][3];
                q_to_m(palm.orientation, m);
                // (a joint: -z to the fingertips, +y out of the back of the hand, +x to the right. The grip pose of
                // rt_hands.h from it: left hand columns (-y, z, -x), right hand (y, z, x))
                const double sx = h ? 1.0 : -1.0;
                for (int r = 0; r < 3; ++r) g[r][0] = sx * m[r][1], g[r][1] = m[r][2], g[r][2] = sx * m[r][0];
                m_to_q(g, out.q);
                out.p[0] = palm.position.x, out.p[1] = palm.position.y, out.p[2] = palm.position.z;
                out.aim_q[0] = palm.orientation.x, out.aim_q[1] = palm.orientation.y, out.aim_q[2] = palm.orientation.z, out.aim_q[3] = palm.orientation.w;
                std::memcpy(out.aim_p, out.p, sizeof out.p);
                static const int first[5] = {XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_INDEX_METACARPAL_EXT, XR_HAND_JOINT_MIDDLE_METACARPAL_EXT,
                                             XR_HAND_JOINT_RING_METACARPAL_EXT, XR_HAND_JOINT_LITTLE_METACARPAL_EXT};
                out.curl[0][0] = 0.15f, out.curl[0][1] = bend(j[first[0]], j[first[0] + 1]), out.curl[0][2] = bend(j[first[0] + 1], j[first[0] + 2]);
                for (int f = 1; f < 5; ++f)
                    for (int k = 0; k < 3; ++k) out.curl[f][k] = bend(j[first[f] + k], j[first[f] + k + 1]);
                const XrVector3f& a = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
                const XrVector3f& b = j[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position;
                const double pinch = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
                out.trigger = (float)std::clamp((0.045 - pinch) / 0.030, 0.0, 1.0);
                float fist = 0;
                for (int f = 2; f < 5; ++f) fist += out.curl[f][0] + out.curl[f][1];
                out.squeeze = std::clamp((fist / 3.0f - 0.9f) / 1.3f, 0.0f, 1.0f);
                if (hand_prev_t[h] && now - hand_prev_t[h] < 100000000 && now > hand_prev_t[h]) {
                    const double dt = (now - hand_prev_t[h]) / 1e9;
                    for (int k = 0; k < 3; ++k) out.vel[k] = (out.p[k] - hand_prev[h][k]) / dt;
                }
                std::memcpy(hand_prev[h], out.p, sizeof out.p), hand_prev_t[h] = now;
                out.tracked = true, out.fingers = true, out.time = now;
                st_hand_bare++;
            }
        }
        if (!out.tracked && space_grip[h] && actions_ok && focused) {
            XrSpaceVelocity v{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            loc.next = &v;
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (XR_SUCCEEDED(xrLocateSpace(space_grip[h], space_app, t, &loc)) && (loc.locationFlags & need) == need) {
                out.q[0] = loc.pose.orientation.x, out.q[1] = loc.pose.orientation.y, out.q[2] = loc.pose.orientation.z, out.q[3] = loc.pose.orientation.w;
                out.p[0] = loc.pose.position.x, out.p[1] = loc.pose.position.y, out.p[2] = loc.pose.position.z;
                if (v.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) out.vel[0] = v.linearVelocity.x, out.vel[1] = v.linearVelocity.y, out.vel[2] = v.linearVelocity.z;
                if (v.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) out.ang[0] = v.angularVelocity.x, out.ang[1] = v.angularVelocity.y, out.ang[2] = v.angularVelocity.z;
                std::memcpy(out.aim_q, out.q, sizeof out.q), std::memcpy(out.aim_p, out.p, sizeof out.p);
                XrSpaceLocation al{XR_TYPE_SPACE_LOCATION};
                if (space_aim[h] && XR_SUCCEEDED(xrLocateSpace(space_aim[h], space_app, t, &al)) && (al.locationFlags & need) == need) {
                    out.aim_q[0] = al.pose.orientation.x, out.aim_q[1] = al.pose.orientation.y, out.aim_q[2] = al.pose.orientation.z, out.aim_q[3] = al.pose.orientation.w;
                    out.aim_p[0] = al.pose.position.x, out.aim_p[1] = al.pose.position.y, out.aim_p[2] = al.pose.position.z;
                }
                out.trigger = pad_val[h], out.squeeze = pad_val[2 + h];
                out.tracked = true, out.time = now;
                st_hand_ctrl++;
            }
        }
        hands::set_hand(h, out);
    }
}

// The controllers, once a headset frame. What a Touch controller lacks beside a gamepad (the cross, the - button) is
// reached by holding the LEFT stick's click: the right stick then gives the cross, the right stick's click gives -,
// the menu button changes the mode (VR libre / big screen) and Y the person (first / third). The menu button alone
// is +; held: recentre.
void read_pad() {
    uint64_t b = 0;
    int32_t s[4] = {};
    bool active = false;
    if (actions_ok && focused) {
        XrActiveActionSet as{action_set, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &as;
        const XrResult r = xrSyncActions(session, &si);
        if (r == XR_SUCCESS) {
            bool btn[7] = {};
            float val[4] = {};
            XrVector2f st[2] = {};
            for (int i = 0; i < 7; ++i) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a_btn[i];
                XrActionStateBoolean v{XR_TYPE_ACTION_STATE_BOOLEAN};
                if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &v)) && v.isActive) btn[i] = v.currentState != 0, active = true;
            }
            for (int i = 0; i < 4; ++i) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a_val[i];
                XrActionStateFloat v{XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &v)) && v.isActive) val[i] = v.currentState, active = true;
            }
            for (int i = 0; i < 2; ++i) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a_stick[i];
                XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
                if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive) st[i] = v.currentState, active = true;
            }
            std::memcpy(pad_val, val, sizeof pad_val);
            const int64_t now = now_ns();
            // (by their letters, as the game's own prompts show them: on a Switch pad A is the right button, B the lower
            // one, X the upper one, Y the left one)
            if (btn[0]) b |= 1ull << PAD_EAST;
            if (btn[1]) b |= 1ull << PAD_SOUTH;
            if (btn[2]) b |= 1ull << PAD_NORTH;
            if (btn[3]) b |= 1ull << PAD_WEST;
            // (the trigger of the hand that draws the bow is the item's button, ZR: the left one for a left-handed player)
            const int item = hands::main_hand() == 0 ? 0 : 1;
            if (val[1 - item] > 0.5f) b |= 1ull << PAD_LTRIGGER;
            if (val[item] > 0.5f) b |= 1ull << PAD_RTRIGGER;
            // (the grips: with the gestures on they close the hands: the left one raises the shield, the right one holds
            // the sword, rt_hands.cpp; L and R as plain buttons are then reached with the left stick's click held)
            const bool chord = btn[5];
            if (!hands::gestures_on() || chord) {
                if (val[2] > 0.5f) b |= 1ull << PAD_LSHOULDER;
                if (val[3] > 0.5f) b |= 1ull << PAD_RSHOULDER;
            }
            auto axis = [](float v) {
                const float a = std::fabs(v);
                if (a < 0.24f) return 0;   // (the dead zone of a gamepad's stick)
                return (int)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
            };
            s[0] = axis(st[0].x), s[1] = axis(st[0].y);
            if (chord) {
                if (!lclick_was) lclick_chorded = false;
                if (st[1].y > 0.5f) b |= 1ull << PAD_UP, lclick_chorded = true;
                if (st[1].y < -0.5f) b |= 1ull << PAD_DOWN, lclick_chorded = true;
                if (st[1].x < -0.5f) b |= 1ull << PAD_LEFT, lclick_chorded = true;
                if (st[1].x > 0.5f) b |= 1ull << PAD_RIGHT, lclick_chorded = true;
                if (btn[6]) b |= 1ull << PAD_BACK, lclick_chorded = true;
                if (hands::gestures_on() && (val[2] > 0.5f || val[3] > 0.5f)) lclick_chorded = true;
                if (btn[4]) {
                    lclick_chorded = true;
                    if (!mode_chord_done) mode_chord_done = true, vr::set_mode(vr::mode() ? 0 : 1);
                } else {
                    mode_chord_done = false;
                }
                if (btn[3]) {   // (with Y: first person / third person)
                    lclick_chorded = true;
                    if (!person_chord_done) person_chord_done = true, vr::set_person(vr::person() == 1 ? 3 : 1);
                    b &= ~(1ull << PAD_WEST);
                } else {
                    person_chord_done = false;
                }
                if (btn[1]) {   // (with B: the video recorder starts or ends; felt in both hands, twice for a start)
                    lclick_chorded = true;
                    if (!rec_chord_done) rec_chord_done = true, rec_toggle();
                    b &= ~(1ull << PAD_SOUTH);
                } else {
                    rec_chord_done = false;
                }
            } else {
                s[2] = axis(st[1].x), s[3] = axis(st[1].y);
                if (btn[6]) b |= 1ull << PAD_RSTICK;
                // (the left click let go without anything else: the game's own "left stick pressed", a short press)
                if (lclick_was && !lclick_chorded) lclick_pulse_until = now + 120000000;
                // the menu button: let go early it is +, held 0.6 s it recentres
                if (btn[4] && !menu_was) menu_down_at = now, menu_long_done = false;
                if (btn[4] && !menu_long_done && now - menu_down_at > 600000000) menu_long_done = true, recentre();
                if (!btn[4] && menu_was && !menu_long_done) start_pulse_until = now + 120000000;
            }
            if (now < start_pulse_until) b |= 1ull << PAD_START;
            if (now < lclick_pulse_until) b |= 1ull << PAD_LSTICK;
            lclick_was = chord, menu_was = btn[4] && !chord;
        }
    }
    if (actions_ok && focused && active) {   // (the game's vibration, kept up while the game asks for it)
        const int64_t now = now_ns();
        // (the recorder's own sign: two short pulses when a recording starts, one long when it ends)
        const int64_t since = now - buzz_at.load(std::memory_order_relaxed);
        const int bk = buzz_kind.load(std::memory_order_relaxed);
        const bool buzz = bk == 2 ? (since < 110000000 || (since > 220000000 && since < 330000000)) : bk == 1 ? since < 450000000 : false;
        for (int h = 0; h < 2; ++h) {
            float a = rumble_amp[h].load(std::memory_order_relaxed);
            if (now - rumble_time[h].load(std::memory_order_relaxed) > 150000000) a = 0.0f;
            if (buzz) a = std::max(a, 0.7f);
            if (a < 0.03f) continue;
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
            v.duration = 40000000;   // (40 ms: renewed at each of the headset's pictures)
            v.frequency = XR_FREQUENCY_UNSPECIFIED;
            v.amplitude = std::clamp(a, 0.0f, 1.0f);
            XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = a_haptic;
            hi.subactionPath = hand_path[h];
            if (XR_SUCCEEDED(xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader*)&v))) st_rumbles++;
        }
    }
    std::lock_guard lk(pad_mu);
    pad_buttons = active ? b : 0, pad_active = active;
    std::memcpy(pad_stick, s, sizeof s);
}

void poll_events() {
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(instance, &ev) != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* e = (const XrEventDataSessionStateChanged*)&ev;
            state = e->state;
            log_host("xr: session state %d", (int)state);
            focused = state == XR_SESSION_STATE_FOCUSED;
            // (the first time the headset is on the head: where it is then becomes "straight ahead", not where it lay)
            if (focused && !ever_focused) ever_focused = true, centred = false;
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (!bad(xrBeginSession(session, &bi), "xrBeginSession")) running = true, frame_begun = false;
            } else if (state == XR_SESSION_STATE_STOPPING) {
                running = false, frame_begun = false;
                bad(xrEndSession(session), "xrEndSession");
                vr::set_head(nullptr, 0);
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                running = false, frame_begun = false;
                vr::set_head(nullptr, 0);
                closing.store(state == XR_SESSION_STATE_EXITING ? "the headset's runtime ends the game's session" : "the headset is lost");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            running = false, frame_begun = false;
            vr::set_head(nullptr, 0);
            log_host("xr: the headset's runtime is going away");
            closing.store("the headset's runtime is going away");
        }
    }
}


bool make_chain(Chain& c, uint32_t w, uint32_t h) {
    if (c.sc && c.w == w && c.h == h) return true;
    if (c.sc) xrDestroySwapchain(c.sc), c = Chain{};
    if (!chain_format) {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
        std::vector<int64_t> f(n);
        if (n) xrEnumerateSwapchainFormats(session, n, &n, f.data());
        std::string all;
        for (int64_t v : f) all += std::to_string(v) + " ";
        for (int64_t want : {(int64_t)VK_FORMAT_R8G8B8A8_SRGB, (int64_t)VK_FORMAT_R8G8B8A8_UNORM})
            if (!chain_format && std::find(f.begin(), f.end(), want) != f.end()) chain_format = want;
        log_host("xr: swapchain formats offered: %s-> %lld%s", all.c_str(), (long long)chain_format,
                 chain_format == VK_FORMAT_R8G8B8A8_SRGB ? " (8 bits sRGB)" : chain_format ? " (8 bits, not sRGB: colours will be too light)" : " (none we can fill)");
        if (!chain_format) return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = chain_format;
    ci.sampleCount = 1, ci.width = w, ci.height = h, ci.faceCount = 1, ci.arraySize = 1, ci.mipCount = 1;
    if (bad(xrCreateSwapchain(session, &ci, &c.sc), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(c.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> im(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (!n || bad(xrEnumerateSwapchainImages(c.sc, n, &n, (XrSwapchainImageBaseHeader*)im.data()), "xrEnumerateSwapchainImages")) return false;
    for (auto& i : im) c.images.push_back(i.image);
    c.w = w, c.h = h;
    return true;
}

void move_layout(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vk::vkCmdPipelineBarrier2(c, &di);
}

void end_empty() {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frame_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    locked([&] { bad(xrEndFrame(session, &ei), "xrEndFrame (no layer)"); });
    st_no_render++;
}

// A finished picture of the game on its way into the headset's images: acquired, its copy submitted, not released yet.
// Found with the real headset: released as soon as the copy was SUBMITTED, an image could be shown before the
// card had copied into it (our copy waits behind the game's own frame on the one queue): one eye new and the other
// old, or a picture three frames old. Now an image is released only once its copy has finished, and until then the
// headset keeps showing the pair before, with that pair's own pose.
struct Flight {
    bool on = false;
    vr::Pair p;
    int slot = 0, n = 0;
} flight;
vr::Pair shown;          // the pair the headset's images hold (released)
bool ui_shown = false;   // the panel's chain holds a released image too
int64_t copy_wait_ns = 4000000;

bool copy_finished(int64_t wait_ns) { return vk::vkWaitForFences(vk::device, 1, &fence[flight.slot], VK_TRUE, (uint64_t)wait_ns) == VK_SUCCESS; }

void land() {
    locked([&] {
        for (int i = 0; i < flight.n; ++i) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            bad(xrReleaseSwapchainImage(chain[i].sc, &ri), "xrReleaseSwapchainImage");
        }
    });
    shown = flight.p, shown_ok = true;
    if (flight.n == 3) ui_shown = true;
    flight.on = false;
}

void take_off(const vr::Pair& p) {
    const bool with_ui = p.kind == 2 && p.ui_separate && p.ui;
    for (int i = 0; i < (with_ui ? 3 : 2); ++i)
        if (chain[i].sc && (chain[i].w != p.w || chain[i].h != p.h)) shown_ok = false, ui_shown = false;   // (another size: new chains)
    if (!make_chain(chain[0], p.w, p.h) || !make_chain(chain[1], p.w, p.h) || (with_ui && !make_chain(chain[2], p.w, p.h))) return;
    const int n = with_ui ? 3 : 2;
    uint32_t index[3] = {};
    int got = 0;
    locked([&] {
        for (int i = 0; i < n; ++i) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (bad(xrAcquireSwapchainImage(chain[i].sc, &ai, &index[i]), "xrAcquireSwapchainImage")) break;
            ++got;
        }
    });
    // Waited for OUTSIDE the host's queue lock, and never for ever. Found, after an update of the
    // headset: the cable link dropped while the game ran, the headset's runtime never gave an image back, and this wait
    // (then infinite, under the host's lock) held the lock: the game's own thread could no longer submit anything, the
    // whole game froze and its window would not even close. The wait does not touch the queue (XR_KHR_vulkan_enable
    // lists only xrBeginFrame, xrEndFrame, xrAcquireSwapchainImage and xrReleaseSwapchainImage as calls that may).
    int waited = 0;
    for (int i = 0; i < got; ++i) {
        const int64_t t0 = now_ns();
        XrResult r = XR_TIMEOUT_EXPIRED;
        while (r == XR_TIMEOUT_EXPIRED && running && now_ns() - t0 < 1500000000ll) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = 100000000;   // (100 ms a try)
            r = xrWaitSwapchainImage(chain[i].sc, &wi);
        }
        if (r != XR_SUCCESS) {
            if (r == XR_TIMEOUT_EXPIRED) last_error = "xrWaitSwapchainImage: the headset gives no image back", st_errors++;
            else bad(r, "xrWaitSwapchainImage");
            break;
        }
        ++waited;
    }
    if (waited < got) {
        // An image acquired and not waited for cannot be released (the runtime would refuse it): those chains are given
        // up, made again at the next picture, if the headset is still there. Nothing more is sent meanwhile.
        log_host("xr: the headset did not give its images back (link lost?): its chains are made again; the game goes on");
        for (int i = 0; i < 3; ++i)
            if (chain[i].sc) xrDestroySwapchain(chain[i].sc), chain[i] = Chain{};
        shown_ok = false, ui_shown = false;
        st_copy_failed++, st_chains_lost++;
        return;
    }
    const int s2 = slot;
    slot ^= 1;
    bool sent = false;
    if (got == n) {
        vk::vkResetFences(vk::device, 1, &fence[s2]);
        VkCommandBuffer c = cmd[s2];
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk::vkBeginCommandBuffer(c, &bi);
        const VkImage src[3] = {p.eye[0], p.eye[1], p.ui};
        for (int i = 0; i < n; ++i) {
            const VkImage dst = chain[i].images[index[i]];
            move_layout(c, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.extent = {p.w, p.h, 1};
            vk::vkCmdCopyImage(c, src[i], VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            move_layout(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        vk::vkEndCommandBuffer(c);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = c;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        sent = H->queue_submit(&si, fence[s2]) != 0;
    }
    if (!sent) {   // (what was acquired is given back as it is: the pair before stays shown)
        locked([&] {
            for (int i = 0; i < got; ++i) {
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(chain[i].sc, &ri);
            }
        });
        shown_ok = false, ui_shown = false;   // (those images hold nothing sure any more)
        st_copy_failed++;
        return;
    }
    flight.on = true, flight.p = p, flight.slot = s2, flight.n = n;
    st_copies++;
}

// One turn of the headset's thread, after the frame began: the game's last finished picture is sent on its way if it
// is a new one, and the headset's frame ends with the last pair whose copy has finished.
void end_frame() {
    vr::Pair p;
    const bool have = vr::pair_now(&p);
    if (flight.on && copy_finished(0)) land();
    if (!flight.on && have && p.serial != shown.serial && p.w && p.eye[0]) {
        take_off(p);
        if (flight.on) {
            if (copy_finished(copy_wait_ns)) land();
            else st_deferred++;   // (not done in time for this frame: the next one)
        }
    }
    if (!should_render || !shown_ok) return end_empty();
    const vr::Pair& q0 = shown;
    const bool with_ui = q0.kind == 2 && q0.ui_separate && q0.ui && ui_shown;

    // the layers
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad[3] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerCylinderKHR cyl[2] = {{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
    const XrCompositionLayerBaseHeader* layers[4] = {};
    uint32_t nl = 0;
    auto sub = [&](int i) {
        XrSwapchainSubImage si2{};
        si2.swapchain = chain[i].sc;
        si2.imageRect.extent = {(int32_t)q0.w, (int32_t)q0.h};
        return si2;
    };
    // (space_app is changed by this thread alone: no lock here, the game's thread is never kept waiting by a frame's end)
    if (q0.kind == 2) {
        // the head's own view: each eye's picture was made in the field and orientation the headset gave for that eye, on
        // the head as it was when the game drew the frame; it is declared exactly so
        const double* q = q0.head.q;
        for (int e = 0; e < 2; ++e) {
            const vr::EyeGeom& g = q0.eg[e];
            double qe[4], pe[3];
            q_mul(q, g.q, qe);
            q_turn(q, g.p, pe);
            pv[e].pose.orientation = {(float)qe[0], (float)qe[1], (float)qe[2], (float)qe[3]};
            pv[e].pose.position = {(float)(q0.head.p[0] + pe[0]), (float)(q0.head.p[1] + pe[1]), (float)(q0.head.p[2] + pe[2])};
            pv[e].fov.angleLeft = g.fov[0], pv[e].fov.angleRight = g.fov[1], pv[e].fov.angleUp = g.fov[2], pv[e].fov.angleDown = g.fov[3];
            pv[e].subImage = sub(e);
        }
        proj.space = space_app;
        proj.viewCount = 2;
        proj.views = pv;
        layers[nl++] = (const XrCompositionLayerBaseHeader*)&proj;
        st_projection++;
        if (with_ui) {
            // the interface's panel: in front of the head, turning with it only once the head has turned away from it
            const double fx = -2.0 * (q[0] * q[2] + q[3] * q[1]), fz = -(1.0 - 2.0 * (q[0] * q[0] + q[1] * q[1]));
            const double yaw = std::atan2(-fx, -fz);
            if (!panel_set) panel_yaw = yaw, panel_set = true;
            double d = std::remainder(yaw - panel_yaw, 2.0 * kPi);
            const double dead = 20.0 * kPi / 180.0;
            if (std::fabs(d) > dead) panel_yaw += (d - (d > 0 ? dead : -dead)) * 0.08;
            XrCompositionLayerQuad& u = quad[2];
            u.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            u.space = space_app;
            u.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            u.subImage = sub(2);
            u.pose.orientation = {0, (float)std::sin(panel_yaw * 0.5), 0, (float)std::cos(panel_yaw * 0.5)};
            u.pose.position = {(float)(q0.head.p[0] - std::sin(panel_yaw) * ui_dist), (float)(q0.head.p[1] + ui_y), (float)(q0.head.p[2] - std::cos(panel_yaw) * ui_dist)};
            const double w = 2.0 * ui_dist * std::tan(ui_angle * kPi / 360.0);
            u.size = {(float)w, (float)(w * 9.0 / 16.0)};
            layers[nl++] = (const XrCompositionLayerBaseHeader*)&u;
        }
    } else {
        // the big screen: the game's picture (16:9), each eye its own when the scene has depth
        st_screen++;
        for (int e = 0; e < 2; ++e) {
            if (have_cylinder && screen_curved) {
                cyl[e].space = space_app;
                cyl[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                cyl[e].subImage = sub(e);
                cyl[e].pose = identity();
                cyl[e].pose.position = {0, (float)screen_y, 0};
                cyl[e].radius = (float)screen_dist;
                cyl[e].centralAngle = (float)(screen_angle * kPi / 180.0);
                cyl[e].aspectRatio = 16.0f / 9.0f;
                layers[nl++] = (const XrCompositionLayerBaseHeader*)&cyl[e];
            } else {
                quad[e].space = space_app;
                quad[e].eyeVisibility = e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                quad[e].subImage = sub(e);
                quad[e].pose = identity();
                quad[e].pose.position = {0, (float)screen_y, (float)-screen_dist};
                const double w = 2.0 * screen_dist * std::tan(std::min(screen_angle, 150.0) * kPi / 360.0);
                quad[e].size = {(float)w, (float)(w * 9.0 / 16.0)};
                layers[nl++] = (const XrCompositionLayerBaseHeader*)&quad[e];
            }
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frame_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl;
    ei.layers = layers;
    locked([&] { bad(xrEndFrame(session, &ei), "xrEndFrame"); });
    st_frames++;
}

void begin_frame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const int64_t t0 = now_ns();
    frame_begun = false;
    if (bad(xrWaitFrame(session, &wi, &fs), "xrWaitFrame")) return;
    const int64_t dt = now_ns() - t0;
    if (focused) {
        wait_ns_total += dt, wait_n++;
        if (dt > wait_ns_max && wait_n > 60) wait_ns_max = dt;
        // (a turn of this thread much longer than one picture of the headset: a picture of the headset was missed)
        const int64_t now2 = now_ns();
        if (last_turn && fs.predictedDisplayPeriod > 0 && now2 - last_turn > fs.predictedDisplayPeriod * 3 / 2) st_late++;
        last_turn = now2;
    } else {
        last_turn = 0;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    bool ok = true;
    locked([&] { ok = !bad(xrBeginFrame(session, &bi), "xrBeginFrame"); });
    if (!ok) return;
    frame_begun = true;
    {
        std::lock_guard lk(space_mu);
        frame_time = fs.predictedDisplayTime, frame_local = now_ns();
    }
    frame_period = fs.predictedDisplayPeriod, should_render = fs.shouldRender != 0;
    if (!centred) recentre();
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (space_app && XR_SUCCEEDED(xrLocateSpace(space_view, space_app, frame_time, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        vr::Pose h;
        h.q[0] = loc.pose.orientation.x, h.q[1] = loc.pose.orientation.y, h.q[2] = loc.pose.orientation.z, h.q[3] = loc.pose.orientation.w;
        h.p[0] = loc.pose.position.x, h.p[1] = loc.pose.position.y, h.p[2] = loc.pose.position.z;
        vr::set_head(&h, (int64_t)frame_time);
    } else {
        vr::set_head(nullptr, 0);
    }
    // the eyes' gap and their fields, as the headset tells them
    XrViewLocateInfo vi{XR_TYPE_VIEW_LOCATE_INFO};
    vi.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vi.displayTime = frame_time;
    vi.space = space_view;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t nv = 0;
    if (XR_SUCCEEDED(xrLocateViews(session, &vi, &vs, 2, &nv, views)) && nv == 2 && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        const double dx = views[1].pose.position.x - views[0].pose.position.x, dy = views[1].pose.position.y - views[0].pose.position.y,
                     dz = views[1].pose.position.z - views[0].pose.position.z;
        eye_ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
        eye_fov[0] = views[0].fov, eye_fov[1] = views[1].fov;
        // the eyes as the headset gives them, against the head: the pictures are made exactly for them
        vr::EyeGeom eg[2];
        for (int e = 0; e < 2; ++e) {
            const XrPosef& vp = views[e].pose;
            eg[e].q[0] = vp.orientation.x, eg[e].q[1] = vp.orientation.y, eg[e].q[2] = vp.orientation.z, eg[e].q[3] = vp.orientation.w;
            eg[e].p[0] = vp.position.x, eg[e].p[1] = vp.position.y, eg[e].p[2] = vp.position.z;
            eg[e].fov[0] = views[e].fov.angleLeft, eg[e].fov[1] = views[e].fov.angleRight, eg[e].fov[2] = views[e].fov.angleUp, eg[e].fov[3] = views[e].fov.angleDown;
            eye_turn[e] = 2.0 * std::atan2(std::sqrt(eg[e].q[0] * eg[e].q[0] + eg[e].q[1] * eg[e].q[1] + eg[e].q[2] * eg[e].q[2]), std::fabs(eg[e].q[3])) * 180.0 / kPi;
        }
        if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && eg[0].fov[1] > eg[0].fov[0] && eg[1].fov[1] > eg[1].fov[0]) vr::set_eyes(eg);
    }
    read_pad();
    read_hands();
    // the keys: G recentres, V changes the mode (while the game's window is in front)
    static const int kc = (int)opt_int("vr_key_center", 'G'), km = (int)opt_int("vr_key_mode", 'V'), kp = (int)opt_int("vr_key_person", 'P');
    static const int kr = (int)opt_int("vr_key_relief", 'O'), kf = (int)opt_int("vr_key_film", 'C');
    static bool was_c = false, was_m = false, was_p = false, was_r = false, was_f = false;
    DWORD pid = 0;
    if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
    const bool front = pid == GetCurrentProcessId();
    const bool dc = front && kc && (GetAsyncKeyState(kc) & 0x8000), dm = front && km && (GetAsyncKeyState(km) & 0x8000);
    if (dc && !was_c) recentre();
    if (dm && !was_m) vr::set_mode(vr::mode() ? 0 : 1);
    const bool dp = front && kp && (GetAsyncKeyState(kp) & 0x8000);
    if (dp && !was_p) vr::set_person(vr::person() == 1 ? 3 : 1);
    const bool dr = front && kr && (GetAsyncKeyState(kr) & 0x8000);
    if (dr && !was_r) vr::set_relief(vr::relief() > 0.5 ? 0.0 : 1.0);   // (the same picture for both eyes, or the eyes' gap)
    const bool df = front && kf && (GetAsyncKeyState(kf) & 0x8000);
    if (df && !was_f) vr::set_film(!vr::film());   // (the camera view for filming, in the game's window)
    static bool was_t = false;
    const int kt = rec::key();
    const bool dk = front && kt && (GetAsyncKeyState(kt) & 0x8000);
    if (dk && !was_t) rec_toggle();   // (the video recorder)
    was_c = dc, was_m = dm, was_p = dp, was_r = dr, was_f = df, was_t = dk;
}

} // namespace

bool on() { return g_boot_ok; }

void recommended(uint32_t* w, uint32_t* h) {
    if (w) *w = rec_w;
    if (h) *h = rec_h;
}

bool boot() {
    if (g_boot_ok) return true;
    const std::string path = dll_dir() + "\\openxr_loader.dll";
    HMODULE m = LoadLibraryA(path.c_str());
    if (!m) return log_host("xr: openxr_loader.dll is not beside retraced.dll: no headset"), false;
    xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)(void*)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr) return log_host("xr: the loader has no xrGetInstanceProcAddr"), false;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    if (!xrEnumerateInstanceExtensionProperties || !xrCreateInstance) return log_host("xr: the loader's first functions are missing"), false;
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)))
        return log_host("xr: no OpenXR runtime answers (is the headset's PC application installed and set as the active runtime?)"), false;
    std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
    if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
    auto has = [&](const char* name) {
        for (auto& e : ext)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    if (!has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) return log_host("xr: the runtime has no XR_KHR_vulkan_enable: no headset"), false;
    std::vector<const char*> want = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    have_cylinder = has(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    have_refresh = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    have_hands = has(XR_EXT_HAND_TRACKING_EXTENSION_NAME) && opt_int("vr_bare_hands", 1) != 0;   // (vr_bare_hands=0: the controllers only)
    have_hand_source = has("XR_EXT_hand_tracking_data_source");
    have_hand_aim = has("XR_FB_hand_tracking_aim");
    if (have_cylinder) want.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (have_refresh) want.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (have_hands) want.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (have_hands && have_hand_source) want.push_back("XR_EXT_hand_tracking_data_source");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "ReTraced VR");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "ReTraced");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)want.size();
    ci.enabledExtensionNames = want.data();
    XrResult r = xrCreateInstance(&ci, &instance);
    if (XR_FAILED(r) && have_hands) {   // (once more without the hands' extensions)
        log_host("xr: xrCreateInstance with the hands' extensions failed (%d): tried again without them", (int)r);
        have_hands = have_hand_source = false;
        want.erase(std::remove_if(want.begin(), want.end(), [](const char* n) { return std::strstr(n, "hand_tracking") != nullptr; }), want.end());
        ci.enabledExtensionCount = (uint32_t)want.size();
        ci.enabledExtensionNames = want.data();
        r = xrCreateInstance(&ci, &instance);
    }
    if (XR_FAILED(r)) return log_host("xr: xrCreateInstance failed (%d): no headset", (int)r), false;
    runtime_loaded.store(true);
    bool missing = false;
#define RT_XR_LOAD(f)                                                                                                 \
    if (XR_FAILED(xrGetInstanceProcAddr(instance, #f, (PFN_xrVoidFunction*)&f)) || !f) missing = true, log_host("xr: function %s missing", #f);
    RT_XR_FUNCS(RT_XR_LOAD)
#undef RT_XR_LOAD
    if (missing) return false;
    if (have_hands) {
        xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&xrCreateHandTrackerEXT);
        xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&xrLocateHandJointsEXT);
    }
    if (have_refresh) {
        xrGetInstanceProcAddr(instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&xrGetDisplayRefreshRateFB);
        xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&xrEnumerateDisplayRefreshRatesFB);
        xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&xrRequestDisplayRefreshRateFB);
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip))) {
        char b[200];
        std::snprintf(b, sizeof b, "%s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                      XR_VERSION_PATCH(ip.runtimeVersion));
        runtime_name = b;
    }
    log_host("xr: runtime %s; hands: %s%s%s; curved screen layer: %s; refresh rate control: %s", runtime_name.c_str(),
             have_hands ? "XR_EXT_hand_tracking offered" : "XR_EXT_hand_tracking NOT offered", have_hand_source ? ", data source" : "",
             have_hand_aim ? ", aim" : "", have_cylinder ? "yes" : "no", have_refresh ? "yes" : "no");
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(instance, &si, &system_id);
    if (XR_FAILED(r)) {
        char name[XR_MAX_RESULT_STRING_SIZE] = "?";
        xrResultToString(instance, r, name);
        log_host("xr: no headset answers (%s): is it switched on and linked to the PC (Link cable or Air Link)?", name);
        last_error = std::string("xrGetSystem: ") + name;
        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemHandTrackingPropertiesEXT hp{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    if (have_hands) sp.next = &hp;
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system_id, &sp))) system_name = sp.systemName;
    hands_supported = have_hands && hp.supportsHandTracking;
    uint32_t nv = 0;
    XrViewConfigurationView vc[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vc)) && nv == 2)
        rec_w = vc[0].recommendedImageRectWidth, rec_h = vc[0].recommendedImageRectHeight;
    auto strings = [&](PFN_xrGetVulkanInstanceExtensionsKHR fn, std::string& out) {
        uint32_t cap = 0;
        if (XR_FAILED(fn(instance, system_id, 0, &cap, nullptr)) || !cap) return;
        std::vector<char> b(cap);
        if (XR_SUCCEEDED(fn(instance, system_id, cap, &cap, b.data()))) out = b.data();
    };
    strings(xrGetVulkanInstanceExtensionsKHR, vk_inst_ext);
    strings((PFN_xrGetVulkanInstanceExtensionsKHR)xrGetVulkanDeviceExtensionsKHR, vk_dev_ext);
    inst_names = split(vk_inst_ext), dev_names = split(vk_dev_ext);
    log_host("xr: headset \"%s\", %u x %u advised for one eye; Vulkan asks for [%s] and [%s]", system_name.c_str(), rec_w, rec_h,
             vk_inst_ext.c_str(), vk_dev_ext.c_str());
    screen_dist = std::clamp(opt_f("vr_screen_dist", 3.0), 0.5, 20.0);
    screen_angle = std::clamp(opt_f("vr_screen_size", 110.0), 20.0, 170.0);
    screen_y = opt_f("vr_screen_y", 0.0);
    screen_curved = opt_int("vr_screen_curve", 1) != 0;
    ui_dist = std::clamp(opt_f("vr_ui_dist", 2.0), 0.3, 10.0);
    ui_angle = std::clamp(opt_f("vr_ui_size", 70.0), 10.0, 150.0);
    ui_y = opt_f("vr_ui_y", -0.05);
    predict_ms = opt_f("vr_predict_ms", 30.0);
    copy_wait_ns = (int64_t)(std::clamp(opt_f("vr_copy_wait_ms", 4.0), 0.0, 12.0) * 1e6);
    g_boot_ok = true;
    return true;
}

namespace {
uint32_t give(const std::vector<std::string>& names, const char* const* avail, uint32_t n, const char** out, uint32_t cap, const char* what) {
    uint32_t k = 0;
    for (const std::string& s : names) {
        bool found = false;
        for (uint32_t i = 0; i < n; ++i) found = found || s == avail[i];
        if (found && k < cap) out[k++] = s.c_str();
        else if (!found) log_host("xr: the headset asks for the Vulkan %s extension %s, which this PC does not offer", what, s.c_str());
    }
    return k;
}
} // namespace

uint32_t instance_extensions(const char* const* avail, uint32_t n, const char** out, uint32_t cap) {
    return g_boot_ok ? give(inst_names, avail, n, out, cap, "instance") : 0;
}
uint32_t device_extensions(const char* const* avail, uint32_t n, const char** out, uint32_t cap) {
    return g_boot_ok ? give(dev_names, avail, n, out, cap, "device") : 0;
}

void device_created(const RtHost* host, const RtDevice* d) {
    H = host;
    if (!g_boot_ok || !d || !vk::device) return;
    g_dev = *d;
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (bad(xrGetVulkanGraphicsRequirementsKHR(instance, system_id, &req), "xrGetVulkanGraphicsRequirementsKHR")) return;
    log_host("xr: the headset's runtime takes Vulkan %u.%u to %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported),
             XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    VkPhysicalDevice want = VK_NULL_HANDLE;
    if (bad(xrGetVulkanGraphicsDeviceKHR(instance, system_id, d->instance, &want), "xrGetVulkanGraphicsDeviceKHR")) return;
    if (want != d->physical) return void(log_host("xr: the headset is on another graphics card than the game's: no headset"));
    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = d->instance, gb.physicalDevice = d->physical, gb.device = d->device, gb.queueFamilyIndex = d->queue_family, gb.queueIndex = 0;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &gb;
    ci.systemId = system_id;
    bool ok = true;
    locked([&] { ok = !bad(xrCreateSession(instance, &ci, &session), "xrCreateSession"); });
    if (!ok) return void(session = XR_NULL_HANDLE);
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = identity();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ok = !bad(xrCreateReferenceSpace(session, &rs, &space_base), "xrCreateReferenceSpace (local)");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    ok = ok && !bad(xrCreateReferenceSpace(session, &rs, &space_view), "xrCreateReferenceSpace (view)");
    ok = ok && make_app_space(identity());
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = d->queue_family;
    ok = ok && vk::vkCreateCommandPool(vk::device, &pi, nullptr, &pool) == VK_SUCCESS;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 2;
    ok = ok && vk::vkAllocateCommandBuffers(vk::device, &ai, cmd) == VK_SUCCESS;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < 2 && ok; ++i) ok = vk::vkCreateFence(vk::device, &fi, nullptr, &fence[i]) == VK_SUCCESS;
    if (!ok) {
        log_host("xr: the session could not be set up: no headset");
        xrDestroySession(session);
        session = XR_NULL_HANDLE;
        return;
    }
    make_actions();
    if (have_refresh && xrGetDisplayRefreshRateFB) {
        xrGetDisplayRefreshRateFB(session, &refresh_hz);
        uint32_t n = 0;
        std::string all;
        if (xrEnumerateDisplayRefreshRatesFB && XR_SUCCEEDED(xrEnumerateDisplayRefreshRatesFB(session, 0, &n, nullptr)) && n) {
            std::vector<float> rates(n);
            xrEnumerateDisplayRefreshRatesFB(session, n, &n, rates.data());
            for (float v : rates) all += std::to_string((int)(v + 0.5f)) + " ";
        }
        const double ask = opt_f("vr_hz", 0);
        if (ask > 0 && xrRequestDisplayRefreshRateFB && !bad(xrRequestDisplayRefreshRateFB(session, (float)ask), "xrRequestDisplayRefreshRateFB"))
            refresh_hz = (float)ask;
        log_host("xr: the headset shows %.0f pictures a second (it offers: %s)", refresh_hz, all.c_str());
    }
    log_host("xr: session made; controllers %s", actions_ok ? "ready" : "NOT ready");
    std::thread(loop).detach();
}

namespace {
// The link with the headset, closed at once when the headset's runtime ends it (the player quit from the headset's
// menu, the cable link dropped, the runtime is going away), not at the game's exit. Found, after an
// update of the headset: the cable link dropped while the game ran; the game, left with its session to the very end,
// could not even close: at its exit the runtime's own libraries waited for their service, whose threads were gone
// (the one thread left was stuck in XR_EXT_hand_tracking.dll and RuntimeIPCServiceClient_64.dll). The game goes on,
// flat, on the desk's screen; to see it in the headset again, the game is started again.
void teardown(const char* why) {
    log_host("xr: %s: the link with the headset is closed; the game goes on, on the desk's screen (start it again for the headset)", why);
    running = false, focused = false, frame_begun = false, should_render = false;
    vr::set_head(nullptr, 0);
    {
        std::lock_guard lk(pad_mu);
        pad_buttons = 0, pad_active = false;
    }
    // (our copies into the headset's images: done, or given a second)
    if (fence[0] && fence[1]) vk::vkWaitForFences(vk::device, 2, fence, VK_TRUE, 1000000000ull);
    {   // (the game's thread asks for the head under this lock: none of it is in use once it is taken)
        std::lock_guard lk(space_mu);
        space_app = space_view = space_base = XR_NULL_HANDLE;   // (destroyed with the session)
        frame_time = 0;
    }
    for (Chain& c : chain) c = Chain{};
    for (int h = 0; h < 2; ++h) tracker[h] = XR_NULL_HANDLE, space_grip[h] = space_aim[h] = XR_NULL_HANDLE;
    actions_ok = false, flight.on = false, shown_ok = false, ui_shown = false;
    if (session) xrDestroySession(session), session = XR_NULL_HANDLE;
    action_set = XR_NULL_HANDLE;   // (destroyed with the instance)
    if (instance) xrDestroyInstance(instance), instance = XR_NULL_HANDLE;
    st_teardowns++;
    log_host("xr: the link with the headset is closed");
}

// The headset's own thread: one turn per picture the headset shows (72 to 120 a second), whatever the game's pace.
// Each turn: the head's pose for the game, the controllers, and the last picture the game finished (copied into the
// headset's images when it is a new one; the same images again otherwise, which the headset turns to the head).
// The game never waits for the headset; the queue is shared under the host's lock.
void loop() {
    log_host("xr: the headset's thread runs");
    thread_on.store(true);
    for (;;) {
        poll_events();
        if (const char* why = closing.load()) {   // (the session is over for good: closed now, and this thread ends)
            teardown(why);
            torn_down.store(true);
            return;
        }
        if (!running) {
            Sleep(50);
            continue;
        }
        begin_frame();
        if (!frame_begun) {
            Sleep(5);
            continue;
        }
        end_frame();
        frame_begun = false;
    }
}
} // namespace

// The game exits (its window closed, or the game quit): the link with the headset is closed first, while the
// runtime's service still answers. Found: when the cable link had dropped (the session idle, not
// ended), the game, closed by the player, stayed for ever in the runtime's libraries at the process's exit. The
// headset's thread closes it (the session is its own); the caller waits for it, wait_ms at most. Returns 1 when the
// link is closed (or there was none), 0 when the headset's thread did not answer in time: the host then ends the
// process at once rather than wait in the runtime's libraries.
// The game exits after a headset's runtime was loaded: the process is ended without unloading its libraries. Seen:
// the game's window closed, the link with the headset closed in time, then the process stayed alive
// for good with one thread inside NVIDIA's Vulkan driver, called from vulkan-1.dll as the libraries were unloaded
// (read with a stack tool); the next start, beside it, prepared its pipelines for minutes instead of 2 seconds.
// Nothing is lost: Longshot saves its caches during play, not at the exit.
int exit_hard() { return runtime_loaded.load() ? 1 : 0; }

int shutdown(int wait_ms) {
    if (!instance && !session) return 1;
    if (!thread_on.load()) {   // (no headset's thread: nothing else uses the instance)
        teardown("the game exits");
        return 1;
    }
    const char* none = nullptr;
    closing.compare_exchange_strong(none, "the game exits");
    const int64_t until = now_ns() + (int64_t)wait_ms * 1000000;
    while (!torn_down.load() && now_ns() < until) Sleep(5);
    return torn_down.load() ? 1 : 0;
}

// The head's pose for the game, asked at the moment the game builds its view (the game's thread): for the time the
// headset gave its last frame, moved on by what has passed since and by predict_ms more.
bool locate_head(vr::Pose* out, int64_t* time) {
    if (!running || !focused || predict_ms < 0) return false;
    std::lock_guard lk(space_mu);
    if (!space_app || !space_view || !frame_time) return false;
    const XrTime t = frame_time + (XrTime)(now_ns() - frame_local) + (XrTime)(predict_ms * 1e6);
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space_view, space_app, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        return false;
    out->q[0] = loc.pose.orientation.x, out->q[1] = loc.pose.orientation.y, out->q[2] = loc.pose.orientation.z, out->q[3] = loc.pose.orientation.w;
    out->p[0] = loc.pose.position.x, out->p[1] = loc.pose.position.y, out->p[2] = loc.pose.position.z;
    *time = (int64_t)t;
    st_located++;
    return true;
}

void rumble(int side, float heavy, float light) {
    if (side < 0 || side > 1) return;
    rumble_amp[side].store(std::max(heavy, light), std::memory_order_relaxed);
    rumble_time[side].store(now_ns(), std::memory_order_relaxed);
}

int pad(uint64_t* buttons, int32_t stick[4]) {
    std::lock_guard lk(pad_mu);
    if (!pad_active) return 0;
    *buttons = pad_buttons;
    std::memcpy(stick, pad_stick, sizeof pad_stick);
    return 1;
}

std::string report_json() {
    char b[1800];
    std::snprintf(b, sizeof b,
                  "{\"headset\": %s, \"runtime\": \"%s\", \"system\": \"%s\", \"advised_eye\": [%u, %u], \"session\": %s, \"state\": %d, \"running\": %s,\n"
                  "    \"hands_offered\": %s, \"hands_data_source\": %s, \"hands_aim\": %s, \"curved_screen\": %s, \"refresh_hz\": %.1f,\n"
                  "    \"frames_sent\": %llu, \"pictures_copied\": %llu, \"copies_shown_a_frame_later\": %llu, \"copies_failed\": %llu, \"chains_lost\": %llu, \"link_closed\": %llu, \"headset_pictures_missed\": %llu, \"head_asked_by_the_game\": %llu, \"frames_head_view\": %llu, \"frames_screen\": %llu, \"frames_without_picture\": %llu,\n"
                  "    \"wait_ms\": {\"mean\": %.3f, \"max\": %.3f}, \"period_ms\": %.3f, \"eyes_gap_mm\": %.2f, \"eyes_turned_degrees\": [%.2f, %.2f],\n"
                  "    \"eye_fields_degrees\": [[%.1f, %.1f, %.1f, %.1f], [%.1f, %.1f, %.1f, %.1f]], \"swapchain_format\": %lld,\n"
                  "    \"controllers\": %s, \"controllers_held\": %s, \"vibrations\": %llu, \"recentres\": %llu, \"errors\": %llu, \"last_error\": \"%s\"}",
                  g_boot_ok ? "true" : "false", json_escape(runtime_name).c_str(), json_escape(system_name).c_str(), rec_w, rec_h,
                  session ? "true" : "false", (int)state, running ? "true" : "false", have_hands ? "true" : "false",
                  have_hand_source ? "true" : "false", have_hand_aim ? "true" : "false", have_cylinder ? "true" : "false", refresh_hz,
                  (unsigned long long)st_frames, (unsigned long long)st_copies, (unsigned long long)st_deferred, (unsigned long long)st_copy_failed, (unsigned long long)st_chains_lost, (unsigned long long)st_teardowns,
                  (unsigned long long)st_late, (unsigned long long)st_located, (unsigned long long)st_projection, (unsigned long long)st_screen, (unsigned long long)st_no_render,
                  wait_n ? wait_ns_total / 1e6 / wait_n : 0.0, wait_ns_max / 1e6, frame_period / 1e6, eye_ipd * 1000.0, eye_turn[0], eye_turn[1],
                  eye_fov[0].angleLeft * 180 / kPi, eye_fov[0].angleRight * 180 / kPi, eye_fov[0].angleUp * 180 / kPi, eye_fov[0].angleDown * 180 / kPi,
                  eye_fov[1].angleLeft * 180 / kPi, eye_fov[1].angleRight * 180 / kPi, eye_fov[1].angleUp * 180 / kPi, eye_fov[1].angleDown * 180 / kPi,
                  (long long)chain_format, actions_ok ? "true" : "false", pad_active ? "true" : "false", (unsigned long long)st_rumbles, (unsigned long long)st_recentres,
                  (unsigned long long)st_errors, json_escape(last_error).c_str());
    std::string out = b;
    char hb[300];
    std::snprintf(hb, sizeof hb,
                  ",\n    \"hands\": {\"controllers_poses\": %s, \"bare_hands_supported\": %s, \"bare_hands_trackers\": %s, \"reads_from_controllers\": %llu, "
                  "\"reads_from_bare_hands\": %llu}}",
                  space_grip[0] && space_grip[1] ? "true" : "false", hands_supported ? "true" : "false", tracker[0] && tracker[1] ? "true" : "false",
                  (unsigned long long)st_hand_ctrl, (unsigned long long)st_hand_bare);
    if (!out.empty() && out.back() == '}') out.pop_back(), out += hb;
    return out;
}

} // namespace rt::xr

// Hook V8 (host/nn_hid.c, where the host maps the game's vibration to a gamepad's two motors): side 0 left, 1 right.
extern "C" __declspec(dllexport) void rt_vr_rumble(int side, float heavy, float light) { rt::xr::rumble(side, heavy, light); }
