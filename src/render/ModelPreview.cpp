#include "render/ModelPreview.h"
#include "render/VulkanHook.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <imgui_impl_vulkan.h>
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

// Item preview. Our own cTkModelResourceRenderer under the HUD's RTT node renders a scene
// built from a scratch customisation, so the real item never changes. The overlay samples its render target.
namespace os::render::preview {

namespace {

using features::Rd;
using features::Wr;
using features::Abs;

using GetRtTexFn = void* (*)(uint32_t res, int32_t index);
using ConstructFn = bool (*)(void* mrr, void* data, uint32_t parent, int32_t w, int32_t h, const char* pipeline, void* cb7, void* cb8, void* cb9);
using UpdateFn = void (*)(void* mrr, float dt, bool flag);
using EnqueueFn = void (*)(void* mrr, uint32_t node, const float* colour, float f, bool b);
using SetResFn = void (*)(void* mrr, uint32_t res, bool force, const void* opts);
using GenTaskFn = void* (*)(void* slot, void* outRes, const char* filename);
using PollFn = int32_t (*)(void* mgr, void* task);
using HudFn = void (*)(void* hud);
using HudUpdateFn = void (*)(void* hud, float dt);
using RebuildFn = void (*)(void* slot);
using CtorFn = void* (*)(void* mrr);

constexpr uint32_t kHud = 0x7330E0, kProcTex = 0x930C90;
int64_t g_hudMrrs = 0x14B430; // HUD.ThumbnailRenderers
constexpr uint32_t kMrrSize = 0x4D0, kMrrData = 0x470, kScale = 0x478, kSync = 0x4C0, kSlotSize = 0x3AE0, kSize = 512;

struct
{
    GetRtTexFn getTex = nullptr;
    ConstructFn construct = nullptr;
    UpdateFn update = nullptr;
    EnqueueFn enqueue = nullptr;
    SetResFn setRes = nullptr;
    GenTaskFn genTask = nullptr;
    PollFn poll = nullptr;
    RebuildFn rebuildDesc = nullptr, rebuildLook = nullptr;
    CtorFn ctor = nullptr;
    void *cb7 = nullptr, *cb8 = nullptr, *cb9 = nullptr;
    std::pair<uintptr_t, uintptr_t> hudConstruct{ 0, 0 };
    int64_t rtOff = 0x454, viewOff = 0x170;
    // cTkModelRendererData size, Camera.Rotate and Camera.Pitch (degrees), from the game's metadata
    uint32_t dataSize = 0xC0, rotOff = 0x38, pitchOff = 0x30;
    uint32_t aspectOff = 0x90, fovOff = 0xA4, lightOff = 0xAC, thumbOff = 0xB0;
    int64_t lightNode = 0x4DE628; // Data offset of the HUD's thumbnail light handle
    bool ok = false;
} F;

safetyhook::InlineHook g_constructHk, g_enqueueHk, g_hudHk, g_hudUpdateHk;

// main thread
std::atomic<uint32_t> g_parent{ 0 };
uint32_t g_builtFor = 0;
uint8_t* g_mrr = nullptr;
uint8_t* g_data = nullptr; // our copy of the HUD's renderer data, so our camera never moves the HUD's
float g_baseRot = 0, g_basePitch = 0;
int32_t g_hudMode = 0;
// One renderer per load. Every extra renderer we made drew black (only the first one drew), so new looks swap
// into it with SetResourceHandle, which drops the old node and rebuilds (Update state 1). Built models are kept
// per item, so switching back skips the build.
struct Model
{
    uint8_t* scratch = nullptr;
    alignas(16) uint8_t smart[16] = {}; // our reference to the built scene
    uint64_t version = 0;
    std::string file;
    bool ready = false;
};
std::map<std::string, Model> g_models; // by item key
Model* g_cur = nullptr;   // the one shown
Model* g_build = nullptr; // the one building
void* g_task = nullptr;
int g_pollFrames = 0;
struct { uint32_t node = 0; float colour[4] = { 0, 0, 0, 0 }; float f = 1.f; bool b = true; bool seen = false; } g_hudArgs;

// requests from the UI
std::mutex g_mx;
Request g_req;
std::atomic<uint64_t> g_reqSerial{ 0 }, g_imageSerial{ 0 };
uint64_t g_doneSerial = 0, g_currentSerial = 0, g_buildSerial = 0;
std::atomic<bool> g_hasImage{ false }, g_wanted{ false };
std::atomic<float> g_scale{ 2.f }, g_yaw{ 0.f }, g_pitch{ 0.f };
std::atomic<float> g_aspect{ 1.f };
std::atomic<uint32_t> g_rt{ 0 };
std::atomic<const char*> g_stage{ "waiting for a load" };

// render thread
VkImageView g_view = VK_NULL_HANDLE;
VkDescriptorSet g_ds = VK_NULL_HANDLE;
std::vector<VkDescriptorSet> g_retiredDescriptors;
bool g_tried = false;

bool SafeConstruct(uint8_t* mrr, uintptr_t data);
bool SafeCtor(uint8_t* mrr);

bool InHudConstruct(uintptr_t ret) { return ret >= F.hudConstruct.first && ret < F.hudConstruct.second; }

bool ConstructHk(void* mrr, void* data, uint32_t parent, int32_t w, int32_t h, const char* pipeline, void* cb7, void* cb8, void* cb9)
{
    if (InHudConstruct((uintptr_t)_ReturnAddress()) && parent != g_parent) {
        g_parent = parent; // the HUD's RTT node, rebuilt on each load
        log::Info("Preview", "HUD render node 0x{:X}", parent);
    }
    return g_constructHk.call<bool>(mrr, data, parent, w, h, pipeline, cb7, cb8, cb9);
}

// The HUD's own render arguments, reused for ours.
void EnqueueHk(void* mrr, uint32_t node, const float* colour, float f, bool b)
{
    if ((uint8_t*)mrr != g_mrr) {
        g_hudArgs.node = node;
        if (colour) memcpy(g_hudArgs.colour, colour, 16);
        g_hudArgs.f = f;
        g_hudArgs.b = b;
        g_hudArgs.seen = true;
    }
    g_enqueueHk.call<void>(mrr, node, colour, f, b);
}

// The constructor is inlined in this build; this is its recipe.
void InitSlot(uint8_t* p)
{
    memset(p, 0, kSlotSize);
    *(uint16_t*)(p + 0x2C) = 0x100;
    *(uint64_t*)(p + 0x38) = 0xC4CEB9FE1A85EC53ull;
    *(uint8_t**)(p + 0x40) = p + 0x58;
    *(uint8_t**)(p + 0x48) = p + 0x58;
    *(uint64_t*)(p + 0x68) = 0x20;
    for (int i = 0; i < 0x42; ++i)
        for (uint32_t base : { 0x90u, 0x1D70u })
            for (int k = 0; k < 20; ++k) *(float*)(p + base + i * 0x70 + k * 4) = 1.f;
    *(float*)(p + 0x3A50) = 1.f;
    *(uint64_t*)(p + 0x3A88) = 0xC4CEB9FE1A85EC53ull;
    *(uint8_t**)(p + 0x3A90) = p + 0x3AA8;
    *(uint8_t**)(p + 0x3A98) = p + 0x3AA8;
    *(uint64_t*)(p + 0x3AB8) = 0x20;
}

bool BuildRenderer()
{
    uintptr_t d = game::Data(), hud = 0;
    // a copy of the renderer data the HUD's own thumbnail renderer was built with
    if (!d || !Rd(d + kHud + g_hudMrrs + kMrrData, hud) || !hud) { g_stage = "no HUD renderer data"; return false; }
    uint32_t size = std::min<uint32_t>(F.dataSize, 0x200);
    uint8_t* data = (uint8_t*)_aligned_malloc(0x200, 16); // one per load, kept: an old renderer may still point at it
    memset(data, 0, 0x200);
    for (uint32_t i = 0; i < size; ++i)
        if (!Rd(hud + i, data[i])) { _aligned_free(data); g_stage = "HUD renderer data unreadable"; return false; }
    // Nothing in the game fills the HUD's block, so Fov and AspectRatio are 0 and the projection draws nothing
    // Fill whatever is still 0.
    auto fill = [&](uint32_t off, float v) { if (off + 4 <= 0x200 && *(float*)(data + off) == 0.f) *(float*)(data + off) = v; };
    fill(F.aspectOff, (float)kSize / kSize);
    fill(F.fovOff, 35.f);
    fill(F.lightOff, 2.5f); // one light only, so a bright one keeps the far side readable
    // ThumbnailMode 1 draws the game's discovery background behind the model.
    // The HUD's mode 0 clears to black, hiding the silhouette of dark paint.
    g_hudMode = 1;
    *(int32_t*)(data + F.thumbOff) = g_hudMode;
    g_data = data;
    g_baseRot = *(float*)(data + F.rotOff);
    g_basePitch = *(float*)(data + F.pitchOff);
    g_mrr = (uint8_t*)_aligned_malloc(kMrrSize, 16); // kept for the load, the game has no teardown we can call
    memset(g_mrr, 0, kMrrSize);
    // the C++ constructor first, as the game does for its own renderers; a zeroed one reached "drawn" and drew nothing
    bool made = SafeCtor(g_mrr);
    bool ok = made && SafeConstruct(g_mrr, (uintptr_t)data);
    uint32_t rt = 0;
    Rd((uintptr_t)g_mrr + F.rtOff, rt);
    log::Info("Preview", "Renderer: ctor {}, construct {}, target 0x{:X}, data 0x{:X} bytes, HUD mode {}", made, ok, rt, size, g_hudMode);
    if (!ok || !rt) { g_mrr = nullptr; g_stage = "renderer could not be made"; return false; }
    g_rt = rt;
    return true;
}

// SEH needs functions without C++ objects, so the game calls that can fault live here.
void* SafeGenTaskRaw(uint8_t* scratch, uint8_t* smart, const char* file)
{
    __try { return F.genTask(scratch, smart, file); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// What every game path runs on a slot before building it (Customise.FreshSlotBuild): the descriptor list from
// the part groups, then the colour and texture tables. Without them the build saw only the seed, so living ships
// came out partial and procedural ones empty.
bool SafeRebuild(uint8_t* slot)
{
    __try {
        if (F.rebuildDesc) F.rebuildDesc(slot);
        if (F.rebuildLook) F.rebuildLook(slot);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeSetRes(uint8_t* mrr, uint32_t res)
{
    alignas(16) uint8_t opts[0xC0] = {}; // SetResourceHandle destroys this block when done
    __try { F.setRes(mrr, res, true, opts); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int32_t SafePoll()
{
    __try { return F.poll((void*)(game::Data() + kProcTex), g_task); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool SafeUpdate(float dt)
{
    __try { F.update(g_mrr, dt, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeRender()
{
    __try {
        // the handle is the HUD's thumbnail light, not a model. Its colour and strength are the HUD's own:
        // a white light at 1.0 left the model black, so the engine's light units are larger than that.
        static const float white[4] = { 1.f, 1.f, 1.f, 1.f };
        uint32_t light = *(uint32_t*)(game::Data() + F.lightNode);
        bool hud = g_hudArgs.seen && g_hudArgs.f > 0.f;
        F.enqueue(g_mrr, light ? light : g_hudArgs.node, hud ? g_hudArgs.colour : white, hud ? g_hudArgs.f : 1.f, g_hudArgs.b);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeCtor(uint8_t* mrr)
{
    if (!F.ctor) return false;
    __try { F.ctor(mrr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The player scene can be drawn with the renderer's built-in animation components.
// Its gameplay components expect a live player and must not be instantiated for a preview.
void PreviewComponents()
{
    if (g_cur && g_cur->file == "MODELS/COMMON/PLAYER/PLAYERCHARACTER/PLAYERCHARACTER.SCENE.MBIN") return;
    reinterpret_cast<void (*)()>(F.cb7)();
}

bool SafeConstruct(uint8_t* mrr, uintptr_t data)
{
    __try { return F.construct(mrr, (void*)data, g_parent, kSize, kSize, "PIPELINES/PIPELINENGUISCREENVIEW.BIN", (void*)&PreviewComponents, F.cb8, F.cb9); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Swaps a built model into the renderer.
bool Use(Model* m, uint64_t serial)
{
    uint32_t res = *(uint32_t*)m->smart;
    if (!SafeSetRes(g_mrr, res)) { g_stage = "model swap faulted"; log::Warn("Preview", "SetResourceHandle faulted on 0x{:X}", res); return false; }
    g_cur = m;
    g_currentSerial = serial;
    g_stage = "rendering";
    return true;
}

// Scene creation and camera updates belong in simulation, before the engine prepares render data.
void Drive(float dt)
{
    if (!g_wanted || !g_parent) return;
    if (g_builtFor != g_parent) { // a load rebuilt the HUD node our renderer hung under
        g_models.clear();
        g_cur = g_build = nullptr;
        g_mrr = nullptr; g_rt = 0; g_hasImage = false; g_task = nullptr; g_doneSerial = 0;
        g_builtFor = g_parent;
    }
    if (!g_mrr && !BuildRenderer()) { g_wanted = false; return; }

    // start a scene for the newest request
    Request req;
    uint64_t serial;
    {
        std::lock_guard lk(g_mx);
        req = g_req;
        serial = g_reqSerial;
    }
    if (req.file.empty()) {
        g_cur = nullptr;
        g_hasImage = false;
        g_stage = "no model file for this item";
    }
    if (!g_task && serial != g_doneSerial && !req.file.empty()) {
        g_doneSerial = serial;
        g_cur = nullptr;
        g_hasImage = false;
        auto it = g_models.find(req.key);
        if (it != g_models.end() && it->second.ready && it->second.version == req.version && it->second.file == req.file) {
            Use(&it->second, serial); // built before with this look: show it again
        }
        else {
            // an older model for this item is replaced; its scene stays referenced, the game has no free we can call
            Model& m = g_models[req.key];
            if (g_cur == &m) g_cur = nullptr;
            m = Model{};
            m.version = req.version;
            m.file = req.file;
            m.scratch = (uint8_t*)_aligned_malloc(kSlotSize, 16);
            InitSlot(m.scratch);
            if (req.fill) req.fill(m.scratch);
            bool rebuilt = SafeRebuild(m.scratch);
            log::Info("Preview", "Slot for {}: {} part groups, rebuild {}", req.key, *(uint32_t*)(m.scratch + 0x3A74), rebuilt ? "done" : "faulted");
            g_build = &m;
            g_buildSerial = serial;
            g_task = SafeGenTaskRaw(m.scratch, m.smart, req.file.c_str());
            g_pollFrames = 0;
            g_stage = g_task ? "building the model" : "model build failed";
            if (g_task) log::Info("Preview", "Building {} from {} ({} models)", req.key, req.file, g_models.size());
            else { log::Warn("Preview", "Generation task failed for {}", req.file); g_build = nullptr; }
        }
    }
    if (g_task) {
        int32_t r = SafePoll();
        ++g_pollFrames;
        if (r == 1) {
            uint32_t res = *(uint32_t*)g_build->smart;
            g_task = nullptr;
            if (!res || res == 0xFFFFFFFF) { g_stage = "model build gave no scene"; log::Warn("Preview", "No scene after {} frames", g_pollFrames); }
            else {
                g_build->ready = true;
                log::Info("Preview", "Model ready after {} frames, resource 0x{:X}", g_pollFrames, res);
                if (g_buildSerial == serial) Use(g_build, serial);
            }
            g_build = nullptr;
        }
        else if (r < 0 || g_pollFrames > 60 * 30) {
            g_task = nullptr;
            g_build = nullptr;
            g_stage = "model build timed out";
            log::Warn("Preview", "Model build {} after {} frames", r < 0 ? "faulted" : "timed out", g_pollFrames);
        }
    }
    if (!g_cur) return;
    Wr<float>((uintptr_t)g_mrr + kScale, g_scale.load());
    *(float*)(g_data + F.aspectOff) = g_aspect.load();
    // turntable: our own data block, which the camera reads every frame
    *(float*)(g_data + F.rotOff) = g_baseRot + g_yaw.load();
    *(float*)(g_data + F.pitchOff) = std::clamp(g_basePitch + g_pitch.load(), -80.f, 80.f);
    if (!SafeUpdate(dt)) { g_stage = "model update faulted"; g_wanted = false; }
}

void HudUpdateHk(void* hud, float dt)
{
    g_hudUpdateHk.call<void>(hud, dt);
    Drive(dt);
}

// Ours goes first. The original queues the HUD, ship, Multi-Tool and pet thumbnails, then ends by resetting the
// render target (tail jump to Engine::EnqueueSetRenderTarget). A draw queued after that reset never reached our
// target; the game's own thumbnails are all queued before it.
void HudHk(void* hud)
{
    if (g_wanted && g_cur && g_mrr && g_builtFor == g_parent && g_currentSerial == g_reqSerial.load()) {
        uint32_t state = 0;
        Rd((uintptr_t)g_mrr + kSync, state);
        if (!SafeRender()) { g_stage = "render step faulted"; g_wanted = false; }
        else if (state == 8) { // EnqueueRender only submits geometry in this state.
            g_imageSerial = g_currentSerial;
            g_hasImage = true;
        }
    }
    g_hudHk.call<void>(hud);
}

void* CallGet(uint32_t rt, int32_t index = 0)
{
    __try { return F.getTex(rt, index); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void Init()
{
    g_tried = true;
    auto& L = features::Loc();
    auto fn = [&](const char* n) -> void* { const auto& t = L.Resolve(n); return t.ok ? (void*)Abs(t.func) : nullptr; };
    auto glob = [&](const char* n) -> void* { const auto& t = L.Resolve(n); return t.ok && t.global ? (void*)t.global : nullptr; };
    F.getTex = (GetRtTexFn)fn("Engine.GetRenderBufferTexture");
    F.construct = (ConstructFn)fn("MRR.Construct");
    F.ctor = (CtorFn)fn("MRR.Ctor");
    F.update = (UpdateFn)fn("MRR.Update");
    F.enqueue = (EnqueueFn)fn("MRR.EnqueueRender");
    F.setRes = (SetResFn)fn("MRR.SetResourceHandle");
    F.genTask = (GenTaskFn)fn("Customise.CreateGenerationTask");
    F.poll = (PollFn)fn("ProcTex.PollForTaskCompletion");
    F.rebuildDesc = (RebuildFn)fn("Customise.RebuildDescriptors");
    F.rebuildLook = (RebuildFn)fn("Customise.RebuildLook");
    if (auto* c = hooks::meta::Get("cTkModelRendererData"); c && c->size) F.dataSize = c->size;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "Camera.Rotate")) F.rotOff = m->offset;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "AspectRatio")) F.aspectOff = m->offset;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "Fov")) F.fovOff = m->offset;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "LightIntensityMultiplier")) F.lightOff = m->offset;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "ThumbnailMode")) F.thumbOff = m->offset;
    if (const auto& t = features::Loc().Resolve("HUD.ThumbnailLightNode"); t.ok && t.offset) F.lightNode = *t.offset;
    if (const auto& t = features::Loc().Resolve("HUD.ThumbnailRenderers"); t.ok && t.offset) g_hudMrrs = *t.offset;
    if (auto m = hooks::meta::Path("cTkModelRendererData", "Camera.Pitch")) F.pitchOff = m->offset;
    F.cb7 = glob("Gfx.ModelViewAllowedComponentsCb");
    F.cb8 = glob("Gfx.ModelViewAttachmentPrepareCb");
    F.cb9 = glob("Gfx.ModelViewViewportCb");
    if (const auto& t = L.Resolve("MRR.RenderTarget"); t.ok && t.offset) F.rtOff = *t.offset;
    if (const auto& t = L.Resolve("Texture.ViewField"); t.ok && t.offset) F.viewOff = *t.offset;
    const auto& hc = L.Resolve("HUD.Construct");
    if (hc.ok) {
        auto ranges = features::Img().FuncRanges(hc.func);
        if (!ranges.empty()) F.hudConstruct = { Abs(ranges.front().first), Abs(ranges.front().second) };
        for (auto& r : ranges) F.hudConstruct = { std::min(F.hudConstruct.first, Abs(r.first)), std::max(F.hudConstruct.second, Abs(r.second)) };
    }
    const auto& hq = L.Resolve("HUD.EnqueueRenderThumbnailQueue");
    const auto& hu = L.Resolve("HUD.UpdateThumbnailQueue");
    F.ok = F.getTex && F.ctor && F.construct && F.update && F.enqueue && F.setRes && F.genTask && F.poll && F.cb7 && F.cb8 && F.cb9 && hc.ok && hq.ok && hu.ok;
    if (F.ok) {
        if (auto h = safetyhook::InlineHook::create((void*)F.construct, (void*)&ConstructHk)) g_constructHk = std::move(*h);
        if (auto h = safetyhook::InlineHook::create((void*)F.enqueue, (void*)&EnqueueHk)) g_enqueueHk = std::move(*h);
        if (auto h = safetyhook::InlineHook::create((void*)Abs(hq.func), (void*)&HudHk)) g_hudHk = std::move(*h);
        if (auto h = safetyhook::InlineHook::create((void*)Abs(hu.func), (void*)&HudUpdateHk)) g_hudUpdateHk = std::move(*h);
        F.ok = (bool)g_constructHk && (bool)g_enqueueHk && (bool)g_hudHk && (bool)g_hudUpdateHk;
    }
    log::Info("Preview", "Item preview {}", F.ok ? "ready" : "unavailable");
}

// The image of one render target as an ImGui texture. Render thread.
ImTextureID Show(uint32_t rt, Info& now, bool own)
{
    uintptr_t tex = (uintptr_t)CallGet(rt);
    if (!tex) { now.step = "no texture yet"; return ImTextureID{}; }
    now.tex = tex;
    uint32_t sType = 0;
    if (Rd(tex + 0x90, sType) && sType == 14) { Rd(tex + 0xA8, now.format); Rd(tex + 0xAC, now.width); Rd(tex + 0xB0, now.height); }
    uint64_t view = 0;
    if (!Rd(tex + F.viewOff, view) || !view) { now.step = "no image view"; return ImTextureID{}; }
    // Our target only holds the model inside the frame that draws it; by present time its memory holds other
    // work. So it is copied into our own image right after the game's pass,
    // and we show the copy, with alpha read as 1 since the model pass leaves it at 0.
    if (own) {
        if (!CaptureTarget(view, now.width, now.height)) { now.step = "copy could not be made"; return ImTextureID{}; }
        auto srcs = CaptureSources();
        uint64_t copy = 0;
        // by default the pass that also has a depth buffer, the one that draws the model
        for (auto& s : srcs) if (!copy && s.depth) copy = s.view;
        for (auto& s : srcs) if (!copy) copy = s.view;
        if (!copy) { now.step = "waiting for the game's pass"; return ImTextureID{}; }
        view = copy;
    }
    if ((VkImageView)view != g_view) {
        // BuildFrame runs while older frames may still sample this descriptor.
        // Retire it with the capture resources at device teardown.
        if (g_ds) g_retiredDescriptors.push_back(g_ds);
        g_view = (VkImageView)view;
        g_ds = ImGui_ImplVulkan_AddTexture(g_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        log::Info("Preview", "Target 0x{:X}: view 0x{:X}, {}x{}", rt, view, now.width, now.height);
    }
    return (ImTextureID)g_ds;
}

}

void Install()
{
    if (!g_tried) Init();
}

void Want(const Request& r)
{
    std::lock_guard lk(g_mx);
    if (r.file == g_req.file && r.key == g_req.key && r.version == g_req.version) return;
    g_req = r;
    ++g_reqSerial;
    g_hasImage = false;
}

void SetScale(float s) { g_scale = s; }
void SetAspect(float aspect) { g_aspect = std::clamp(aspect, 0.1f, 10.f); }

void SetView(float yaw, float pitch)
{
    g_yaw = yaw;
    g_pitch = pitch;
}

ImTextureID Image(Info* info)
{
    if (!g_tried) Init();
    Info now;
    g_wanted = true;
    ImTextureID img{};
    if (F.ok && g_hasImage && g_rt && g_imageSerial.load() == g_reqSerial.load()) {
        img = Show(g_rt, now, true);
        now.step = img ? "item preview" : now.step;
        now.own = img != ImTextureID{};
    }
    if (!img) now.step = F.ok ? g_stage.load() : "preview unavailable";
    if (info) *info = now;
    return img;
}

void Release()
{
    if (g_ds) ImGui_ImplVulkan_RemoveTexture(g_ds);
    for (auto ds : g_retiredDescriptors) ImGui_ImplVulkan_RemoveTexture(ds);
    g_retiredDescriptors.clear();
    g_ds = VK_NULL_HANDLE;
    g_view = VK_NULL_HANDLE;
}

}
