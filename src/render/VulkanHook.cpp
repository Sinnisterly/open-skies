#include "render/VulkanHook.h"
#include "core/AppLog.h"
#include "render/Overlay.h"
#include "render/TexturesVk.h"
#include "render/ModelPreview.h"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <safetyhook.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace os::render {

namespace {

struct Frame
{
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;
};

struct Chain
{
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t minImages = 2;
    HWND hwnd = nullptr;
    std::vector<VkImage> images;
    std::vector<Frame> frames;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
};

struct Fn
{
#define VKFN(n) PFN_vk##n n = nullptr;
    VKFN(CreateRenderPass) VKFN(DestroyRenderPass) VKFN(CreateImageView) VKFN(DestroyImageView)
    VKFN(CreateFramebuffer) VKFN(DestroyFramebuffer) VKFN(CreateCommandPool) VKFN(DestroyCommandPool)
    VKFN(AllocateCommandBuffers) VKFN(CreateFence) VKFN(DestroyFence) VKFN(CreateSemaphore) VKFN(DestroySemaphore)
    VKFN(WaitForFences) VKFN(ResetFences) VKFN(ResetCommandBuffer) VKFN(BeginCommandBuffer) VKFN(EndCommandBuffer)
    VKFN(CmdBeginRenderPass) VKFN(CmdEndRenderPass) VKFN(QueueSubmit) VKFN(GetSwapchainImagesKHR) VKFN(DeviceWaitIdle)
    VKFN(AllocateMemory) VKFN(CmdPipelineBarrier)
    VKFN(CreateImage) VKFN(DestroyImage) VKFN(GetImageMemoryRequirements) VKFN(BindImageMemory) VKFN(FreeMemory)
    VKFN(CmdCopyImage)
#undef VKFN
};

std::recursive_mutex g_mx;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
uint32_t g_api = VK_API_VERSION_1_1;
std::vector<VkQueueFamilyProperties> g_families;
Fn fn;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;

std::unordered_map<VkSurfaceKHR, HWND> g_surfaces;
std::unordered_map<VkQueue, uint32_t> g_queueFamily;
std::unordered_map<VkSwapchainKHR, Chain> g_chains;
VkRenderPass g_rp = VK_NULL_HANDLE;
VkFormat g_rpFormat = VK_FORMAT_UNDEFINED;
bool g_imgui = false;
uint64_t g_presents = 0;
std::string g_note;

// Real device level entry points captured from vkGetDeviceProcAddr.
PFN_vkQueuePresentKHR n_present = nullptr;
PFN_vkCreateSwapchainKHR n_createSwap = nullptr;
PFN_vkDestroySwapchainKHR n_destroySwap = nullptr;
PFN_vkGetDeviceQueue n_getQueue = nullptr;
PFN_vkGetDeviceQueue2 n_getQueue2 = nullptr;
PFN_vkDestroyDevice n_destroyDevice = nullptr;
PFN_vkCreateImageView n_createView = nullptr;
PFN_vkDestroyImageView n_destroyView = nullptr;

// Every image view the game makes, with what it was made from, so a copy can be made with a different swizzle.
struct ViewInfo { VkImage image; VkImageViewType type; VkFormat format; VkComponentMapping comp; VkImageSubresourceRange range; };
std::mutex g_viewMx;
std::unordered_map<VkImageView, ViewInfo> g_views;

void NoteView(const VkImageViewCreateInfo* ci, VkImageView v)
{
    if (!ci || !v) return;
    std::lock_guard lk(g_viewMx);
    g_views[v] = { ci->image, ci->viewType, ci->format, ci->components, ci->subresourceRange };
}

void ForgetView(VkImageView v)
{
    std::lock_guard lk(g_viewMx);
    g_views.erase(v);
}

// Pass capture (CaptureTarget). Framebuffers and render passes are remembered when made, so a pass's begin call
// tells which images it draws to and the layout it leaves them in. More than one kind of pass writes the target
// each frame, so each kind (render pass, attachment count, depth or not) gets its own copy.
struct CopySlot
{
    VkRenderPass rp = VK_NULL_HANDLE; // null for dynamic rendering
    uint32_t count = 0, index = 0;
    bool depth = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool inited = false; // a copy has been recorded, so the image is in SHADER_READ_ONLY_OPTIMAL
};
constexpr int kCopySlots = 4;
struct Capture
{
    VkImage src = VK_NULL_HANDLE;
    VkImageSubresourceRange range{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping comp{};
    uint32_t width = 0, height = 0, memType = UINT32_MAX;
    CopySlot slots[kCopySlots];
    int used = 0;
};
std::mutex g_capMx;
Capture g_cap;
// A game command buffer may be recorded but not yet submitted when the UI changes targets,
// so copy resources live until device teardown.
struct RetiredCopy { VkImageView view; VkImage image; VkDeviceMemory memory; };
std::vector<RetiredCopy> g_retiredCopies;
std::atomic<VkImage> g_capSrc{ VK_NULL_HANDLE };
std::unordered_map<VkFramebuffer, std::vector<VkImageView>> g_fbs;
std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> g_rpLayouts;
struct Pending { VkImageLayout layout; int slot; };
std::unordered_map<VkCommandBuffer, Pending> g_capPending;

bool IsDepth(VkImageView v)
{
    std::lock_guard lk(g_viewMx);
    auto it = g_views.find(v);
    return it != g_views.end() && (it->second.range.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
}

VkImage ImageOf(VkImageView v)
{
    std::lock_guard lk(g_viewMx);
    auto it = g_views.find(v);
    return it == g_views.end() ? VK_NULL_HANDLE : it->second.image;
}

void NoteView(const VkImageViewCreateInfo* ci, VkImageView v);

// Our own image and view (alpha read as 1, the model pass leaves it at 0) for one slot. g_capMx held.
bool MakeCopy(CopySlot& s)
{
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = g_cap.format;
    ii.extent = { g_cap.width, g_cap.height, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!g_device || fn.CreateImage(g_device, &ii, nullptr, &s.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    fn.GetImageMemoryRequirements(g_device, s.img, &req);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, g_cap.memType };
    if (!(req.memoryTypeBits & (1u << g_cap.memType)) || fn.AllocateMemory(g_device, &ai, nullptr, &s.mem) != VK_SUCCESS) {
        fn.DestroyImage(g_device, s.img, nullptr);
        s.img = VK_NULL_HANDLE;
        return false;
    }
    fn.BindImageMemory(g_device, s.img, s.mem, 0);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = s.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = g_cap.format;
    vi.components = g_cap.comp;
    vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (fn.CreateImageView(g_device, &vi, nullptr, &s.view) != VK_SUCCESS) {
        fn.DestroyImage(g_device, s.img, nullptr);
        fn.FreeMemory(g_device, s.mem, nullptr);
        s.img = VK_NULL_HANDLE;
        s.mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void NotePass(VkCommandBuffer cmd, const VkImageView* views, uint32_t count, const VkImageLayout* layouts, VkRenderPass rp, int depth)
{
    VkImage src = g_capSrc.load();
    for (uint32_t i = 0; i < count; ++i) {
        if (!views[i] || ImageOf(views[i]) != src) continue;
        bool hasDepth = depth > 0;
        if (depth < 0) for (uint32_t k = 0; k < count; ++k) hasDepth = hasDepth || (views[k] && IsDepth(views[k]));
        VkImageLayout layout = layouts ? layouts[i] : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::lock_guard lk(g_capMx);
        if (g_cap.src != src) return;
        int slot = -1;
        for (int k = 0; k < g_cap.used && slot < 0; ++k) {
            CopySlot& sl = g_cap.slots[k];
            if (sl.rp == rp && sl.count == count && sl.index == i && sl.depth == hasDepth && sl.layout == layout) slot = k;
        }
        if (slot < 0 && g_cap.used < kCopySlots) {
            CopySlot& sl = g_cap.slots[g_cap.used];
            sl.rp = rp;
            sl.count = count;
            sl.index = i;
            sl.depth = hasDepth;
            sl.layout = layout;
            if (MakeCopy(sl)) {
                slot = g_cap.used++;
            }
        }
        if (slot < 0) return;
        g_capPending[cmd] = { layout, slot };
        return;
    }
}

void NoteRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* bi, VkSubpassContents contents)
{
    if (!g_capSrc.load() || !bi) return;
    std::vector<VkImageView> views;
    std::vector<VkImageLayout> layouts;
    for (auto* p = (const VkBaseInStructure*)bi->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) { // imageless framebuffer
            auto* a = (const VkRenderPassAttachmentBeginInfo*)p;
            views.assign(a->pAttachments, a->pAttachments + a->attachmentCount);
        }
    {
        std::lock_guard lk(g_capMx);
        if (views.empty()) if (auto it = g_fbs.find(bi->framebuffer); it != g_fbs.end()) views = it->second;
        if (auto it = g_rpLayouts.find(bi->renderPass); it != g_rpLayouts.end()) layouts = it->second;
    }
    layouts.resize(views.size(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    NotePass(cmd, views.data(), (uint32_t)views.size(), layouts.data(), bi->renderPass, -1);
}

void NoteRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri)
{
    if (!g_capSrc.load() || !ri) return;
    std::vector<VkImageView> views;
    std::vector<VkImageLayout> layouts;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) {
        views.push_back(ri->pColorAttachments[i].imageView);
        layouts.push_back(ri->pColorAttachments[i].imageLayout);
    }
    bool depth = ri->pDepthAttachment && ri->pDepthAttachment->imageView;
    NotePass(cmd, views.data(), (uint32_t)views.size(), layouts.data(), VK_NULL_HANDLE, depth ? 1 : 0);
}

// After the pass on cmd has ended: our target -> that pass's copy, both left in the layouts the readers expect.
void CopyAfterPass(VkCommandBuffer cmd)
{
    // Keep this generation stable through all barrier and copy recording calls.
    std::lock_guard lk(g_capMx);
    auto it = g_capPending.find(cmd);
    if (it == g_capPending.end()) return;
    Pending pd = it->second;
    g_capPending.erase(it);
    if (!fn.CmdCopyImage || g_cap.src != g_capSrc.load() || pd.slot >= g_cap.used) return;
    VkImage dst = g_cap.slots[pd.slot].img;
    bool inited = g_cap.slots[pd.slot].inited;
    g_cap.slots[pd.slot].inited = true;
    VkImageSubresourceRange r = g_cap.range;
    r.levelCount = 1;
    r.layerCount = 1;
    VkImageMemoryBarrier b[2]{};
    for (auto& x : b) { x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; }
    b[0].subresourceRange = r;
    b[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b[0].image = g_cap.src;
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = pd.layout;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[1].image = dst;
    b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = inited ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    VkImageCopy c{};
    c.srcSubresource = { r.aspectMask, r.baseMipLevel, r.baseArrayLayer, 1 };
    c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent = { g_cap.width, g_cap.height, 1 };
    fn.CmdCopyImage(cmd, g_cap.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    std::swap(b[0].oldLayout, b[0].newLayout);
    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
}

void ForgetCapture()
{
    g_capSrc = VK_NULL_HANDLE;
    std::lock_guard lk(g_capMx);
    for (int k = 0; k < g_cap.used && g_device; ++k) {
        CopySlot& s = g_cap.slots[k];
        g_retiredCopies.push_back({ s.view, s.img, s.mem });
    }
    g_cap.~Capture();
    new (&g_cap) Capture();
    g_capPending.clear();
}

safetyhook::InlineHook h_createFb, h_destroyFb, h_createRp, h_createRp2, h_beginRp, h_beginRp2, h_endRp, h_endRp2, h_beginRd, h_endRd;
PFN_vkCreateFramebuffer n_createFb = nullptr;
PFN_vkDestroyFramebuffer n_destroyFb = nullptr;
PFN_vkCreateRenderPass n_createRp = nullptr;
PFN_vkCreateRenderPass2 n_createRp2 = nullptr, n_createRp2K = nullptr;
PFN_vkCmdBeginRenderPass n_beginRp = nullptr;
PFN_vkCmdBeginRenderPass2 n_beginRp2 = nullptr, n_beginRp2K = nullptr;
PFN_vkCmdEndRenderPass n_endRp = nullptr;
PFN_vkCmdEndRenderPass2 n_endRp2 = nullptr, n_endRp2K = nullptr;
PFN_vkCmdBeginRendering n_beginRd = nullptr, n_beginRdK = nullptr;
PFN_vkCmdEndRendering n_endRd = nullptr, n_endRdK = nullptr;

void NoteFb(const VkFramebufferCreateInfo* ci, VkFramebuffer fb)
{
    if (!ci || !fb || (ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) return;
    std::lock_guard lk(g_capMx);
    g_fbs[fb].assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
}
void ForgetFb(VkFramebuffer fb) { std::lock_guard lk(g_capMx); g_fbs.erase(fb); }
void NoteRp(const VkRenderPassCreateInfo* ci, VkRenderPass rp)
{
    if (!ci || !rp) return;
    std::lock_guard lk(g_capMx);
    auto& v = g_rpLayouts[rp];
    v.clear();
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) v.push_back(ci->pAttachments[i].finalLayout);
}
void NoteRp2(const VkRenderPassCreateInfo2* ci, VkRenderPass rp)
{
    if (!ci || !rp) return;
    std::lock_guard lk(g_capMx);
    auto& v = g_rpLayouts[rp];
    v.clear();
    for (uint32_t i = 0; i < ci->attachmentCount; ++i) v.push_back(ci->pAttachments[i].finalLayout);
}

// Each call has an export version (hooked in vulkan-1.dll) and a version handed out by vkGetDeviceProcAddr.
VkResult VKAPI_CALL Exp_CreateFb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* o)
{ VkResult r = h_createFb.call<VkResult>(d, ci, a, o); if (r == VK_SUCCESS && o) NoteFb(ci, *o); return r; }
VkResult VKAPI_CALL Wrap_CreateFb(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a, VkFramebuffer* o)
{ VkResult r = n_createFb(d, ci, a, o); if (r == VK_SUCCESS && o) NoteFb(ci, *o); return r; }
void VKAPI_CALL Exp_DestroyFb(VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks* a) { ForgetFb(fb); h_destroyFb.call<void>(d, fb, a); }
void VKAPI_CALL Wrap_DestroyFb(VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks* a) { ForgetFb(fb); n_destroyFb(d, fb, a); }
VkResult VKAPI_CALL Exp_CreateRp(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* o)
{ VkResult r = h_createRp.call<VkResult>(d, ci, a, o); if (r == VK_SUCCESS && o) NoteRp(ci, *o); return r; }
VkResult VKAPI_CALL Wrap_CreateRp(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a, VkRenderPass* o)
{ VkResult r = n_createRp(d, ci, a, o); if (r == VK_SUCCESS && o) NoteRp(ci, *o); return r; }
VkResult VKAPI_CALL Exp_CreateRp2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* o)
{ VkResult r = h_createRp2.call<VkResult>(d, ci, a, o); if (r == VK_SUCCESS && o) NoteRp2(ci, *o); return r; }
VkResult VKAPI_CALL Wrap_CreateRp2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* o)
{ VkResult r = n_createRp2(d, ci, a, o); if (r == VK_SUCCESS && o) NoteRp2(ci, *o); return r; }
VkResult VKAPI_CALL Wrap_CreateRp2K(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a, VkRenderPass* o)
{ VkResult r = n_createRp2K(d, ci, a, o); if (r == VK_SUCCESS && o) NoteRp2(ci, *o); return r; }

VkSubpassContents Contents(const VkSubpassBeginInfo* s) { return s ? s->contents : VK_SUBPASS_CONTENTS_INLINE; }
void VKAPI_CALL Exp_BeginRp(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, VkSubpassContents s) { h_beginRp.call<void>(c, bi, s); NoteRenderPass(c, bi, s); }
void VKAPI_CALL Wrap_BeginRp(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, VkSubpassContents s) { n_beginRp(c, bi, s); NoteRenderPass(c, bi, s); }
void VKAPI_CALL Exp_BeginRp2(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* s) { h_beginRp2.call<void>(c, bi, s); NoteRenderPass(c, bi, Contents(s)); }
void VKAPI_CALL Wrap_BeginRp2(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* s) { n_beginRp2(c, bi, s); NoteRenderPass(c, bi, Contents(s)); }
void VKAPI_CALL Wrap_BeginRp2K(VkCommandBuffer c, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* s) { n_beginRp2K(c, bi, s); NoteRenderPass(c, bi, Contents(s)); }

void VKAPI_CALL Exp_EndRp(VkCommandBuffer c) { h_endRp.call<void>(c); CopyAfterPass(c); }
void VKAPI_CALL Wrap_EndRp(VkCommandBuffer c) { n_endRp(c); CopyAfterPass(c); }
void VKAPI_CALL Exp_EndRp2(VkCommandBuffer c, const VkSubpassEndInfo* e) { h_endRp2.call<void>(c, e); CopyAfterPass(c); }
void VKAPI_CALL Wrap_EndRp2(VkCommandBuffer c, const VkSubpassEndInfo* e) { n_endRp2(c, e); CopyAfterPass(c); }
void VKAPI_CALL Wrap_EndRp2K(VkCommandBuffer c, const VkSubpassEndInfo* e) { n_endRp2K(c, e); CopyAfterPass(c); }
void VKAPI_CALL Exp_BeginRd(VkCommandBuffer c, const VkRenderingInfo* ri) { h_beginRd.call<void>(c, ri); NoteRendering(c, ri); }
void VKAPI_CALL Wrap_BeginRd(VkCommandBuffer c, const VkRenderingInfo* ri) { n_beginRd(c, ri); NoteRendering(c, ri); }
void VKAPI_CALL Wrap_BeginRdK(VkCommandBuffer c, const VkRenderingInfo* ri) { n_beginRdK(c, ri); NoteRendering(c, ri); }
void VKAPI_CALL Exp_EndRd(VkCommandBuffer c) { h_endRd.call<void>(c); CopyAfterPass(c); }
void VKAPI_CALL Wrap_EndRd(VkCommandBuffer c) { n_endRd(c); CopyAfterPass(c); }
void VKAPI_CALL Wrap_EndRdK(VkCommandBuffer c) { n_endRdK(c); CopyAfterPass(c); }

safetyhook::InlineHook h_createInstance, h_createDevice, h_destroyDevice, h_gdpa, h_present, h_createSwap, h_destroySwap,
    h_getQueue, h_getQueue2, h_createSurface, h_createView, h_destroyView;

thread_local int t_depth = 0;

struct Depth { Depth() { ++t_depth; } ~Depth() { --t_depth; } };

void LoadDeviceFns(VkDevice d)
{
    auto gdpa = h_gdpa.original<PFN_vkGetDeviceProcAddr>();
#define LD(n) fn.n = (PFN_vk##n)gdpa(d, "vk" #n);
    LD(CreateRenderPass) LD(DestroyRenderPass) LD(CreateImageView) LD(DestroyImageView)
    LD(CreateFramebuffer) LD(DestroyFramebuffer) LD(CreateCommandPool) LD(DestroyCommandPool)
    LD(AllocateCommandBuffers) LD(CreateFence) LD(DestroyFence) LD(CreateSemaphore) LD(DestroySemaphore)
    LD(WaitForFences) LD(ResetFences) LD(ResetCommandBuffer) LD(BeginCommandBuffer) LD(EndCommandBuffer)
    LD(CmdBeginRenderPass) LD(CmdEndRenderPass) LD(QueueSubmit) LD(GetSwapchainImagesKHR) LD(DeviceWaitIdle)
    LD(AllocateMemory) LD(CmdPipelineBarrier)
    LD(CreateImage) LD(DestroyImage) LD(GetImageMemoryRequirements) LD(BindImageMemory) LD(FreeMemory)
    LD(CmdCopyImage)
#undef LD
}

bool EnsureRenderPass(VkFormat format)
{
    if (g_rp && g_rpFormat == format) return true;
    if (g_rp) { fn.DestroyRenderPass(g_device, g_rp, nullptr); g_rp = VK_NULL_HANDLE; }
    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1; ci.pAttachments = &att;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 1; ci.pDependencies = &dep;
    if (fn.CreateRenderPass(g_device, &ci, nullptr, &g_rp) != VK_SUCCESS) { log::Error("Vulkan", "Render pass creation failed"); return false; }
    g_rpFormat = format;
    if (g_imgui) {
        ImGui_ImplVulkan_PipelineInfo pi{};
        pi.RenderPass = g_rp;
        pi.Subpass = 0;
        pi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
    }
    return true;
}

void DestroyFrames(Chain& c)
{
    std::vector<VkFence> fences;
    for (auto& f : c.frames) if (f.fence) fences.push_back(f.fence);
    if (!fences.empty()) fn.WaitForFences(c.device, (uint32_t)fences.size(), fences.data(), VK_TRUE, 1'000'000'000ull);
    for (auto& f : c.frames) {
        if (f.fb) fn.DestroyFramebuffer(c.device, f.fb, nullptr);
        if (f.view) fn.DestroyImageView(c.device, f.view, nullptr);
        if (f.fence) fn.DestroyFence(c.device, f.fence, nullptr);
        if (f.sem) fn.DestroySemaphore(c.device, f.sem, nullptr);
    }
    c.frames.clear();
    if (c.pool) { fn.DestroyCommandPool(c.device, c.pool, nullptr); c.pool = VK_NULL_HANDLE; }
}

bool BuildChain(VkDevice d, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& ci)
{
    if (d != g_device || !fn.GetSwapchainImagesKHR) return false;
    Chain c;
    c.device = d;
    c.format = ci.imageFormat;
    c.extent = ci.imageExtent;
    c.minImages = ci.minImageCount;
    auto it = g_surfaces.find(ci.surface);
    c.hwnd = it != g_surfaces.end() ? it->second : nullptr;
    uint32_t n = 0;
    fn.GetSwapchainImagesKHR(d, sc, &n, nullptr);
    c.images.resize(n);
    fn.GetSwapchainImagesKHR(d, sc, &n, c.images.data());
    if (!EnsureRenderPass(c.format)) return false;

    c.frames.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = c.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = c.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (fn.CreateImageView(d, &vi, nullptr, &c.frames[i].view) != VK_SUCCESS) { DestroyFrames(c); return false; }
        VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = g_rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &c.frames[i].view;
        fi.width = c.extent.width; fi.height = c.extent.height; fi.layers = 1;
        if (fn.CreateFramebuffer(d, &fi, nullptr, &c.frames[i].fb) != VK_SUCCESS) { DestroyFrames(c); return false; }
        VkFenceCreateInfo fc{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
        fn.CreateFence(d, &fc, nullptr, &c.frames[i].fence);
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        fn.CreateSemaphore(d, &sci, nullptr, &c.frames[i].sem);
    }
    log::Info("Vulkan", "Swapchain {}x{} format {} images {} window {}", c.extent.width, c.extent.height, (int)c.format, n, (void*)c.hwnd);
    g_chains[sc] = std::move(c);
    if (g_imgui) ImGui_ImplVulkan_SetMinImageCount(std::max<uint32_t>(2u, ci.minImageCount));
    return true;
}

PFN_vkVoidFunction ImGuiLoader(const char* name, void*)
{
    auto gdpa = h_gdpa.original<PFN_vkGetDeviceProcAddr>();
    PFN_vkVoidFunction f = gdpa ? gdpa(g_device, name) : nullptr;
    if (!f && g_gipa) f = g_gipa(g_instance, name);
    return f;
}

bool EnsureImGui(Chain& c, VkQueue q, uint32_t family)
{
    if (g_imgui) return true;
    HWND hwnd = c.hwnd ? c.hwnd : GetForegroundWindow();
    if (!overlay::EnsureContext(hwnd)) return false;
    if (!ImGui_ImplVulkan_LoadFunctions(g_api, &ImGuiLoader)) { g_note = "ImGui could not load Vulkan functions"; log::Error("Vulkan", "{}", g_note); return false; }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = g_api;
    ii.Instance = g_instance;
    ii.PhysicalDevice = g_phys;
    ii.Device = g_device;
    ii.QueueFamily = family;
    ii.Queue = q;
    ii.DescriptorPoolSize = 64 + tex::DescriptorBudget();
    ii.MinImageCount = std::max<uint32_t>(2u, c.minImages);
    ii.ImageCount = std::max<uint32_t>(ii.MinImageCount, (uint32_t)c.images.size());
    ii.PipelineInfoMain.RenderPass = g_rp;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = [](VkResult r) { if (r < 0) log::Warn("Vulkan", "ImGui call returned {}", (int)r); };
    if (!ImGui_ImplVulkan_Init(&ii)) { g_note = "ImGui Vulkan init failed"; log::Error("Vulkan", "{}", g_note); return false; }
    g_imgui = true;
    log::Info("Vulkan", "Overlay ready on queue family {}", family);
    return true;
}

// Draws the overlay into the presented image. Returns true when info was rewritten.
bool RenderOverlay(VkQueue q, const VkPresentInfoKHR* info, VkPresentInfoKHR& out, VkSemaphore& waitSem)
{
    std::lock_guard lk(g_mx);
    ++g_presents;
    if (!g_device || info->swapchainCount == 0) return false;
    auto it = g_chains.find(info->pSwapchains[0]);
    if (it == g_chains.end()) return false;
    Chain& c = it->second;
    uint32_t idx = info->pImageIndices[0];
    if (idx >= c.frames.size()) return false;

    uint32_t family = c.family;
    if (auto qf = g_queueFamily.find(q); qf != g_queueFamily.end()) family = qf->second;
    if (family == UINT32_MAX) family = 0;
    if (family >= g_families.size() || !(g_families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        if (g_note.empty()) { g_note = "Present queue has no graphics support"; log::Warn("Vulkan", "{} (family {})", g_note, family); }
        return false;
    }

    if (!c.pool || c.family != family) {
        DestroyFrames(c);
        c.family = family;
        VkCommandPoolCreateInfo pc{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family };
        if (fn.CreateCommandPool(c.device, &pc, nullptr, &c.pool) != VK_SUCCESS) return false;
        // frames were dropped above, rebuild views and sync objects
        std::vector<VkImage> imgs = c.images;
        c.frames.resize(imgs.size());
        for (size_t i = 0; i < imgs.size(); ++i) {
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = imgs[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = c.format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fn.CreateImageView(c.device, &vi, nullptr, &c.frames[i].view);
            VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fi.renderPass = g_rp; fi.attachmentCount = 1; fi.pAttachments = &c.frames[i].view;
            fi.width = c.extent.width; fi.height = c.extent.height; fi.layers = 1;
            fn.CreateFramebuffer(c.device, &fi, nullptr, &c.frames[i].fb);
            VkFenceCreateInfo fc{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
            fn.CreateFence(c.device, &fc, nullptr, &c.frames[i].fence);
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            fn.CreateSemaphore(c.device, &sci, nullptr, &c.frames[i].sem);
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, c.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
            fn.AllocateCommandBuffers(c.device, &ai, &c.frames[i].cmd);
        }
    }

    if (!EnsureImGui(c, q, family)) return false;

    ImGui_ImplVulkan_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)c.extent.width, (float)c.extent.height);
    if (!overlay::BuildFrame()) return false;

    Frame& f = c.frames[idx];
    if (fn.WaitForFences(c.device, 1, &f.fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) return false;
    fn.ResetFences(c.device, 1, &f.fence);
    fn.ResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    fn.BeginCommandBuffer(f.cmd, &bi);
    tex::Record({ g_instance, g_phys, g_device, h_gdpa.original<PFN_vkGetDeviceProcAddr>(), g_gipa }, f.cmd, idx, (uint32_t)c.frames.size());
    VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = g_rp;
    rb.framebuffer = f.fb;
    rb.renderArea.extent = c.extent;
    fn.CmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    fn.CmdEndRenderPass(f.cmd);
    fn.EndCommandBuffer(f.cmd);

    // ImGui samples game images, so wait for the game's submission first.
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &f.sem;
    if (fn.QueueSubmit(q, 1, &si, f.fence) != VK_SUCCESS) {
        log::Warn("Vulkan", "Overlay submit failed");
        return false;
    }
    out = *info;
    waitSem = f.sem;
    out.waitSemaphoreCount = 1;
    out.pWaitSemaphores = &waitSem;
    return true;
}

template <class Next> VkResult Present(VkQueue q, const VkPresentInfoKHR* info, Next next)
{
    if (t_depth > 0 || !info) return next(q, info);
    Depth d;
    VkPresentInfoKHR copy;
    VkSemaphore sem;
    bool rewritten = false;
    try { rewritten = RenderOverlay(q, info, copy, sem); }
    catch (...) { log::Error("Vulkan", "Overlay threw during present"); }
    return next(q, rewritten ? &copy : info);
}

VkResult VKAPI_CALL Exp_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* i)
{
    return Present(q, i, [](VkQueue a, const VkPresentInfoKHR* b) { return h_present.call<VkResult>(a, b); });
}

VkResult VKAPI_CALL Wrap_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* i)
{
    return Present(q, i, [](VkQueue a, const VkPresentInfoKHR* b) { return n_present(a, b); });
}

template <class Next> VkResult CreateSwap(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* out, Next next)
{
    if (t_depth > 0 || !ci) return next(d, ci, a, out);
    Depth dp;
    VkSwapchainCreateInfoKHR copy = *ci;
    copy.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkResult r = next(d, &copy, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lk(g_mx);
        try { BuildChain(d, *out, copy); }
        catch (...) { log::Error("Vulkan", "Swapchain setup threw"); }
    }
    return r;
}

VkResult VKAPI_CALL Exp_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* o)
{
    return CreateSwap(d, ci, a, o, [](auto... x) { return h_createSwap.call<VkResult>(x...); });
}

VkResult VKAPI_CALL Wrap_CreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* o)
{
    return CreateSwap(d, ci, a, o, [](auto... x) { return n_createSwap(x...); });
}

template <class Next> void DestroySwap(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a, Next next)
{
    if (t_depth == 0) {
        Depth dp;
        std::lock_guard lk(g_mx);
        if (auto it = g_chains.find(sc); it != g_chains.end()) { DestroyFrames(it->second); g_chains.erase(it); }
        next(d, sc, a);
        return;
    }
    next(d, sc, a);
}

void VKAPI_CALL Exp_DestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a)
{
    DestroySwap(d, s, a, [](auto... x) { h_destroySwap.call<void>(x...); });
}

void VKAPI_CALL Wrap_DestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a)
{
    DestroySwap(d, s, a, [](auto... x) { n_destroySwap(x...); });
}

void RecordQueue(VkQueue q, uint32_t family)
{
    std::lock_guard lk(g_mx);
    if (q) g_queueFamily[q] = family;
}

void VKAPI_CALL Exp_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q)
{
    h_getQueue.call<void>(d, fam, idx, q);
    if (q) RecordQueue(*q, fam);
}

void VKAPI_CALL Wrap_GetDeviceQueue(VkDevice d, uint32_t fam, uint32_t idx, VkQueue* q)
{
    n_getQueue(d, fam, idx, q);
    if (q) RecordQueue(*q, fam);
}

void VKAPI_CALL Exp_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* i, VkQueue* q)
{
    h_getQueue2.call<void>(d, i, q);
    if (q && i) RecordQueue(*q, i->queueFamilyIndex);
}

void VKAPI_CALL Wrap_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* i, VkQueue* q)
{
    n_getQueue2(d, i, q);
    if (q && i) RecordQueue(*q, i->queueFamilyIndex);
}

void ReleaseDevice(VkDevice d)
{
    std::lock_guard lk(g_mx);
    if (d != g_device) return;
    for (auto& [sc, c] : g_chains) DestroyFrames(c);
    g_chains.clear();
    ForgetCapture();
    // The device is going away, so nothing the game recorded can still use our resources.
    {
        std::lock_guard ck(g_capMx);
        for (const auto& copy : g_retiredCopies) {
            if (copy.view) fn.DestroyImageView(d, copy.view, nullptr);
            if (copy.image) fn.DestroyImage(d, copy.image, nullptr);
            if (copy.memory) fn.FreeMemory(d, copy.memory, nullptr);
        }
        g_retiredCopies.clear();
    }
    preview::Release();
    tex::Release();
    if (g_imgui) { ImGui_ImplVulkan_Shutdown(); g_imgui = false; }
    if (g_rp) { fn.DestroyRenderPass(d, g_rp, nullptr); g_rp = VK_NULL_HANDLE; }
    g_device = VK_NULL_HANDLE;
    log::Info("Vulkan", "Device released");
}

}

bool CaptureTarget(uint64_t gameView, uint32_t width, uint32_t height)
{
    ViewInfo info;
    {
        std::lock_guard vk(g_viewMx);
        auto it = g_views.find((VkImageView)gameView);
        if (it == g_views.end()) return false;
        info = it->second;
    }
    if (info.type != VK_IMAGE_VIEW_TYPE_2D || !width || !height) return false;
    std::lock_guard lk(g_mx);
    {
        std::lock_guard ck(g_capMx);
        if (g_cap.src == info.image) return true;
    }
    ForgetCapture();
    if (!g_device || !fn.CreateImage || !g_gipa || !g_instance) return false;
    auto memProps = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp{};
    if (memProps) memProps(g_phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) type = i;
    if (type == UINT32_MAX) return false;
    {
        std::lock_guard ck(g_capMx);
        g_cap.src = info.image;
        g_cap.range = info.range;
        g_cap.format = info.format;
        g_cap.comp = info.comp;
        g_cap.width = width;
        g_cap.height = height;
        g_cap.memType = type;
    }
    g_capSrc = info.image;
    return true;
}

std::vector<CaptureSource> CaptureSources()
{
    std::vector<CaptureSource> out;
    std::lock_guard ck(g_capMx);
    for (int k = 0; k < g_cap.used; ++k) {
        const CopySlot& s = g_cap.slots[k];
        out.push_back({ s.inited ? (uint64_t)s.view : 0, s.count, s.index, s.depth, (int)s.layout });
    }
    return out;
}

namespace {

void VKAPI_CALL Exp_DestroyDevice(VkDevice d, const VkAllocationCallbacks* a)
{
    if (t_depth == 0) { Depth dp; ReleaseDevice(d); }
    h_destroyDevice.call<void>(d, a);
}

void VKAPI_CALL Wrap_DestroyDevice(VkDevice d, const VkAllocationCallbacks* a)
{
    if (t_depth == 0) { Depth dp; ReleaseDevice(d); }
    n_destroyDevice(d, a);
}

VkResult VKAPI_CALL Exp_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v)
{
    VkResult r = h_createView.call<VkResult>(d, ci, a, v);
    if (r == VK_SUCCESS && v) NoteView(ci, *v);
    return r;
}

VkResult VKAPI_CALL Wrap_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* v)
{
    VkResult r = n_createView(d, ci, a, v);
    if (r == VK_SUCCESS && v) NoteView(ci, *v);
    return r;
}

void VKAPI_CALL Exp_DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks* a)
{
    ForgetView(v);
    h_destroyView.call<void>(d, v, a);
}

void VKAPI_CALL Wrap_DestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks* a)
{
    ForgetView(v);
    n_destroyView(d, v, a);
}

PFN_vkVoidFunction VKAPI_CALL Hk_GetDeviceProcAddr(VkDevice d, const char* name)
{
    PFN_vkVoidFunction real = h_gdpa.call<PFN_vkVoidFunction>(d, name);
    if (!real || !name) return real;
    if (!strcmp(name, "vkQueuePresentKHR")) { n_present = (PFN_vkQueuePresentKHR)real; return (PFN_vkVoidFunction)&Wrap_QueuePresentKHR; }
    if (!strcmp(name, "vkCreateSwapchainKHR")) { n_createSwap = (PFN_vkCreateSwapchainKHR)real; return (PFN_vkVoidFunction)&Wrap_CreateSwapchainKHR; }
    if (!strcmp(name, "vkDestroySwapchainKHR")) { n_destroySwap = (PFN_vkDestroySwapchainKHR)real; return (PFN_vkVoidFunction)&Wrap_DestroySwapchainKHR; }
    if (!strcmp(name, "vkGetDeviceQueue")) { n_getQueue = (PFN_vkGetDeviceQueue)real; return (PFN_vkVoidFunction)&Wrap_GetDeviceQueue; }
    if (!strcmp(name, "vkGetDeviceQueue2")) { n_getQueue2 = (PFN_vkGetDeviceQueue2)real; return (PFN_vkVoidFunction)&Wrap_GetDeviceQueue2; }
    if (!strcmp(name, "vkDestroyDevice")) { n_destroyDevice = (PFN_vkDestroyDevice)real; return (PFN_vkVoidFunction)&Wrap_DestroyDevice; }
    if (!strcmp(name, "vkCreateImageView")) { n_createView = (PFN_vkCreateImageView)real; return (PFN_vkVoidFunction)&Wrap_CreateImageView; }
    if (!strcmp(name, "vkDestroyImageView")) { n_destroyView = (PFN_vkDestroyImageView)real; return (PFN_vkVoidFunction)&Wrap_DestroyImageView; }
#define CAP(str, var, type, wrap) if (!strcmp(name, str)) { var = (type)real; return (PFN_vkVoidFunction)&wrap; }
    CAP("vkCreateFramebuffer", n_createFb, PFN_vkCreateFramebuffer, Wrap_CreateFb)
    CAP("vkDestroyFramebuffer", n_destroyFb, PFN_vkDestroyFramebuffer, Wrap_DestroyFb)
    CAP("vkCreateRenderPass", n_createRp, PFN_vkCreateRenderPass, Wrap_CreateRp)
    CAP("vkCreateRenderPass2", n_createRp2, PFN_vkCreateRenderPass2, Wrap_CreateRp2)
    CAP("vkCreateRenderPass2KHR", n_createRp2K, PFN_vkCreateRenderPass2, Wrap_CreateRp2K)
    CAP("vkCmdBeginRenderPass", n_beginRp, PFN_vkCmdBeginRenderPass, Wrap_BeginRp)
    CAP("vkCmdBeginRenderPass2", n_beginRp2, PFN_vkCmdBeginRenderPass2, Wrap_BeginRp2)
    CAP("vkCmdBeginRenderPass2KHR", n_beginRp2K, PFN_vkCmdBeginRenderPass2, Wrap_BeginRp2K)
    CAP("vkCmdEndRenderPass", n_endRp, PFN_vkCmdEndRenderPass, Wrap_EndRp)
    CAP("vkCmdEndRenderPass2", n_endRp2, PFN_vkCmdEndRenderPass2, Wrap_EndRp2)
    CAP("vkCmdEndRenderPass2KHR", n_endRp2K, PFN_vkCmdEndRenderPass2, Wrap_EndRp2K)
    CAP("vkCmdBeginRendering", n_beginRd, PFN_vkCmdBeginRendering, Wrap_BeginRd)
    CAP("vkCmdBeginRenderingKHR", n_beginRdK, PFN_vkCmdBeginRendering, Wrap_BeginRdK)
    CAP("vkCmdEndRendering", n_endRd, PFN_vkCmdEndRendering, Wrap_EndRd)
    CAP("vkCmdEndRenderingKHR", n_endRdK, PFN_vkCmdEndRendering, Wrap_EndRdK)
#undef CAP
    return real;
}

VkResult VKAPI_CALL Hk_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out)
{
    VkResult r = h_createInstance.call<VkResult>(ci, a, out);
    if (r == VK_SUCCESS && out && t_depth == 0) {
        std::lock_guard lk(g_mx);
        g_instance = *out;
        if (ci && ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) g_api = ci->pApplicationInfo->apiVersion;
        log::Info("Vulkan", "Instance created, API {}.{}", VK_API_VERSION_MAJOR(g_api), VK_API_VERSION_MINOR(g_api));
    }
    return r;
}

VkResult VKAPI_CALL Hk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out)
{
    VkResult r = h_createDevice.call<VkResult>(phys, ci, a, out);
    if (r != VK_SUCCESS || !out || t_depth > 0) return r;
    std::lock_guard lk(g_mx);
    g_phys = phys;
    g_device = *out;
    LoadDeviceFns(g_device);
    if (g_gipa && g_instance) {
        auto qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t n = 0;
        if (qfp) { qfp(phys, &n, nullptr); g_families.resize(n); qfp(phys, &n, g_families.data()); }
    }
    log::Info("Vulkan", "Device created, {} queue families", g_families.size());
    return r;
}

VkResult VKAPI_CALL Hk_CreateWin32SurfaceKHR(VkInstance i, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSurfaceKHR* s)
{
    VkResult r = h_createSurface.call<VkResult>(i, ci, a, s);
    if (r == VK_SUCCESS && s && ci) {
        std::lock_guard lk(g_mx);
        g_surfaces[*s] = ci->hwnd;
        log::Info("Vulkan", "Surface for window {}", (void*)ci->hwnd);
    }
    return r;
}

template <class Fn> bool Hook(safetyhook::InlineHook& h, HMODULE mod, const char* name, Fn fnp)
{
    void* target = (void*)GetProcAddress(mod, name);
    if (!target) { log::Warn("Vulkan", "{} not exported", name); return false; }
    auto r = safetyhook::InlineHook::create(target, (void*)fnp);
    if (!r) { log::Error("Vulkan", "Hook on {} failed", name); return false; }
    h = std::move(*r);
    return true;
}

}

bool InstallVulkanHooks()
{
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) { log::Error("Vulkan", "vulkan-1.dll not loaded"); return false; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");

    bool ok = true;
    ok &= Hook(h_createInstance, vk, "vkCreateInstance", &Hk_CreateInstance);
    ok &= Hook(h_createDevice, vk, "vkCreateDevice", &Hk_CreateDevice);
    ok &= Hook(h_destroyDevice, vk, "vkDestroyDevice", &Exp_DestroyDevice);
    ok &= Hook(h_gdpa, vk, "vkGetDeviceProcAddr", &Hk_GetDeviceProcAddr);
    ok &= Hook(h_present, vk, "vkQueuePresentKHR", &Exp_QueuePresentKHR);
    ok &= Hook(h_createSwap, vk, "vkCreateSwapchainKHR", &Exp_CreateSwapchainKHR);
    ok &= Hook(h_destroySwap, vk, "vkDestroySwapchainKHR", &Exp_DestroySwapchainKHR);
    ok &= Hook(h_getQueue, vk, "vkGetDeviceQueue", &Exp_GetDeviceQueue);
    Hook(h_getQueue2, vk, "vkGetDeviceQueue2", &Exp_GetDeviceQueue2);
    ok &= Hook(h_createSurface, vk, "vkCreateWin32SurfaceKHR", &Hk_CreateWin32SurfaceKHR);
    Hook(h_createView, vk, "vkCreateImageView", &Exp_CreateImageView);
    Hook(h_destroyView, vk, "vkDestroyImageView", &Exp_DestroyImageView);
    Hook(h_createFb, vk, "vkCreateFramebuffer", &Exp_CreateFb);
    Hook(h_destroyFb, vk, "vkDestroyFramebuffer", &Exp_DestroyFb);
    Hook(h_createRp, vk, "vkCreateRenderPass", &Exp_CreateRp);
    Hook(h_createRp2, vk, "vkCreateRenderPass2", &Exp_CreateRp2);
    Hook(h_beginRp, vk, "vkCmdBeginRenderPass", &Exp_BeginRp);
    Hook(h_beginRp2, vk, "vkCmdBeginRenderPass2", &Exp_BeginRp2);
    Hook(h_endRp, vk, "vkCmdEndRenderPass", &Exp_EndRp);
    Hook(h_endRp2, vk, "vkCmdEndRenderPass2", &Exp_EndRp2);
    Hook(h_beginRd, vk, "vkCmdBeginRendering", &Exp_BeginRd);
    Hook(h_endRd, vk, "vkCmdEndRendering", &Exp_EndRd);
    if (GetModuleHandleW(L"sl.interposer.dll")) log::Info("Vulkan", "Streamline interposer present");
    log::Info("Vulkan", "Hooks {}", ok ? "installed" : "partially installed");
    return ok;
}

Stats GetStats()
{
    std::lock_guard lk(g_mx);
    Stats s;
    s.device = g_device != VK_NULL_HANDLE;
    s.imgui = g_imgui;
    s.presents = g_presents;
    s.note = g_note;
    if (!g_chains.empty()) {
        auto& c = g_chains.begin()->second;
        s.format = (uint32_t)c.format; s.width = c.extent.width; s.height = c.extent.height; s.images = (uint32_t)c.images.size();
    }
    return s;
}

}
