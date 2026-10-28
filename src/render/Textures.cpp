#include "render/Textures.h"
#include "render/TexturesVk.h"
#include "assets/PakIndex.h"
#include "core/AppLog.h"
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace os::render::tex {

namespace {

constexpr uint32_t kMaxDim = 128;              // icons use the first mip at or below this size
constexpr uint32_t kCap = 1200;                // live textures before eviction
constexpr VkDeviceSize kStage = 1u << 20;      // staging bytes per frame slot
constexpr uint32_t kStageSlots = 8;
constexpr uint32_t kSlotsPerBlock = 256;

struct Decoded
{
    std::string path;
    bool ok = false;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
};

struct Tex
{
    VkImage img = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    uint32_t slot = UINT32_MAX;
    VkDeviceMemory own = VK_NULL_HANDLE; // larger images get their own allocation
    uint64_t used = 0;
    bool requested = false, ready = false, failed = false;
};

// Render thread state.
std::unordered_map<std::string, Tex> g_tex;
uint64_t g_frame = 0;

// Decoder thread hand-off.
std::mutex g_qmx;
std::condition_variable g_cv;
std::deque<std::string> g_requests;
std::deque<Decoded> g_done;
bool g_worker = false;

struct Vk
{
#define VKFN(n) PFN_vk##n n = nullptr;
    VKFN(CreateImage) VKFN(DestroyImage) VKFN(GetImageMemoryRequirements) VKFN(AllocateMemory) VKFN(FreeMemory)
    VKFN(BindImageMemory) VKFN(CreateImageView) VKFN(DestroyImageView) VKFN(CreateBuffer) VKFN(DestroyBuffer)
    VKFN(GetBufferMemoryRequirements) VKFN(BindBufferMemory) VKFN(MapMemory) VKFN(UnmapMemory)
    VKFN(CmdPipelineBarrier) VKFN(CmdCopyBufferToImage)
#undef VKFN
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    uint8_t* stageMap = nullptr;
    std::vector<VkDeviceMemory> blocks;
    std::vector<uint32_t> freeSlots;
    VkDeviceSize slotSize = 0;
    uint32_t imageTypeBits = 0;
    bool init = false, broken = false;
} V;

uint32_t Rd32(const std::vector<uint8_t>& d, size_t o)
{
    uint32_t v = 0;
    if (o + 4 <= d.size()) memcpy(&v, d.data() + o, 4);
    return v;
}

bool ParseDds(const std::vector<uint8_t>& d, Decoded& out)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    uint32_t h = Rd32(d, 12), w = Rd32(d, 16), mips = std::max(1u, Rd32(d, 28)), four = Rd32(d, 84);
    size_t off = 128;
    uint32_t block = 16;
    if (four == 0x30315844) { // DX10
        off = 148;
        switch (Rd32(d, 128)) {
        case 98: out.fmt = VK_FORMAT_BC7_UNORM_BLOCK; break;
        case 99: out.fmt = VK_FORMAT_BC7_SRGB_BLOCK; break;
        case 71: out.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; block = 8; break;
        case 72: out.fmt = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; block = 8; break;
        case 77: out.fmt = VK_FORMAT_BC3_UNORM_BLOCK; break;
        case 78: out.fmt = VK_FORMAT_BC3_SRGB_BLOCK; break;
        default: return false;
        }
    }
    else if (four == 0x31545844) { out.fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; block = 8; } // DXT1
    else if (four == 0x35545844) out.fmt = VK_FORMAT_BC3_UNORM_BLOCK;                    // DXT5
    else return false;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
        size_t sz = (size_t)std::max(1u, (mw + 3) / 4) * std::max(1u, (mh + 3) / 4) * block;
        if (std::max(mw, mh) <= kMaxDim) {
            if (off + sz > d.size()) return false;
            out.w = mw;
            out.h = mh;
            out.bytes.assign(d.begin() + off, d.begin() + off + sz);
            return true;
        }
        // no mip at the icon size: fall back to the last one if it is still small enough
        if (m == mips - 1 && std::max(mw, mh) <= kMaxDim * 2 && off + sz <= d.size()) {
            out.w = mw;
            out.h = mh;
            out.bytes.assign(d.begin() + off, d.begin() + off + sz);
            return true;
        }
        off += sz;
    }
    return false; // no mip small enough
}

void Worker()
{
    for (;;) {
        std::string path;
        {
            std::unique_lock lk(g_qmx);
            g_cv.wait(lk, [] { return !g_requests.empty(); });
            path = std::move(g_requests.front());
            g_requests.pop_front();
        }
        Decoded d;
        d.path = path;
        d.ok = ParseDds(assets::paks::Read(path), d);
        std::lock_guard lk(g_qmx);
        g_done.push_back(std::move(d));
    }
}

int MemType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

bool Init(const DeviceInfo& d)
{
    V.init = true;
    V.dev = d.device;
#define LD(n) V.n = (PFN_vk##n)d.gdpa(d.device, "vk" #n); if (!V.n) { V.broken = true; return false; }
    LD(CreateImage) LD(DestroyImage) LD(GetImageMemoryRequirements) LD(AllocateMemory) LD(FreeMemory)
    LD(BindImageMemory) LD(CreateImageView) LD(DestroyImageView) LD(CreateBuffer) LD(DestroyBuffer)
    LD(GetBufferMemoryRequirements) LD(BindBufferMemory) LD(MapMemory) LD(UnmapMemory)
    LD(CmdPipelineBarrier) LD(CmdCopyBufferToImage)
#undef LD
    auto memProps = (PFN_vkGetPhysicalDeviceMemoryProperties)d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties");
    auto fmtProps = (PFN_vkGetPhysicalDeviceFormatProperties)d.gipa(d.instance, "vkGetPhysicalDeviceFormatProperties");
    if (!memProps || !fmtProps) { V.broken = true; return false; }
    memProps(d.phys, &V.mem);
    VkFormatProperties fp{};
    fmtProps(d.phys, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        log::Warn("Icons", "GPU cannot sample BC7, icons disabled");
        V.broken = true;
        return false;
    }

    // Slot size from a probe image of the largest icon size.
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_BC7_UNORM_BLOCK;
    ii.extent = { kMaxDim, kMaxDim, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage probe;
    if (V.CreateImage(V.dev, &ii, nullptr, &probe) != VK_SUCCESS) { V.broken = true; return false; }
    VkMemoryRequirements req;
    V.GetImageMemoryRequirements(V.dev, probe, &req);
    V.DestroyImage(V.dev, probe, nullptr);
    V.slotSize = (req.size + req.alignment - 1) / req.alignment * req.alignment;
    V.imageTypeBits = req.memoryTypeBits;

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = kStage * kStageSlots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (V.CreateBuffer(V.dev, &bi, nullptr, &V.stage) != VK_SUCCESS) { V.broken = true; return false; }
    V.GetBufferMemoryRequirements(V.dev, V.stage, &req);
    int mt = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)mt };
    if (mt < 0 || V.AllocateMemory(V.dev, &ai, nullptr, &V.stageMem) != VK_SUCCESS) { V.broken = true; return false; }
    V.BindBufferMemory(V.dev, V.stage, V.stageMem, 0);
    V.MapMemory(V.dev, V.stageMem, 0, VK_WHOLE_SIZE, 0, (void**)&V.stageMap);
    if (!V.stageMap) { V.broken = true; return false; }

    std::lock_guard lk(g_qmx);
    if (!g_worker) { g_worker = true; std::thread(Worker).detach(); }
    log::Info("Icons", "Texture cache ready, {} KB per icon slot", V.slotSize / 1024);
    return true;
}

uint32_t TakeSlot()
{
    if (V.freeSlots.empty()) {
        int mt = MemType(V.imageTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) mt = MemType(V.imageTypeBits, 0);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, V.slotSize * kSlotsPerBlock, (uint32_t)mt };
        VkDeviceMemory m;
        if (mt < 0 || V.AllocateMemory(V.dev, &ai, nullptr, &m) != VK_SUCCESS) return UINT32_MAX;
        uint32_t base = (uint32_t)V.blocks.size() * kSlotsPerBlock;
        V.blocks.push_back(m);
        for (uint32_t i = kSlotsPerBlock; i-- > 0;) V.freeSlots.push_back(base + i);
    }
    uint32_t s = V.freeSlots.back();
    V.freeSlots.pop_back();
    return s;
}

void Destroy(Tex& t)
{
    if (t.ds) ImGui_ImplVulkan_RemoveTexture(t.ds);
    if (t.view) V.DestroyImageView(V.dev, t.view, nullptr);
    if (t.img) V.DestroyImage(V.dev, t.img, nullptr);
    if (t.slot != UINT32_MAX) V.freeSlots.push_back(t.slot);
    if (t.own) V.FreeMemory(V.dev, t.own, nullptr);
    t = Tex{};
}

void Evict()
{
    size_t live = 0;
    for (auto& [p, t] : g_tex) live += t.ready;
    if (live <= kCap) return;
    std::vector<std::pair<uint64_t, std::string>> old;
    for (auto& [p, t] : g_tex)
        if (t.ready && t.used + 16 < g_frame) old.emplace_back(t.used, p);
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i < old.size() && live > kCap * 9 / 10; ++i, --live) {
        Destroy(g_tex[old[i].second]);
        g_tex.erase(old[i].second);
    }
}

bool Upload(Decoded& d, VkCommandBuffer cmd, uint8_t* stage, VkDeviceSize stageOff)
{
    Tex& t = g_tex[d.path];
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = d.fmt;
    ii.extent = { d.w, d.h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (V.CreateImage(V.dev, &ii, nullptr, &t.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    V.GetImageMemoryRequirements(V.dev, t.img, &req);
    if (req.size > V.slotSize) {
        int mt = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)mt };
        if (mt < 0 || V.AllocateMemory(V.dev, &ai, nullptr, &t.own) != VK_SUCCESS) return false;
        if (V.BindImageMemory(V.dev, t.img, t.own, 0) != VK_SUCCESS) return false;
    }
    else {
        if (V.slotSize % req.alignment || !(req.memoryTypeBits & V.imageTypeBits)) return false;
        t.slot = TakeSlot();
        if (t.slot == UINT32_MAX) return false;
        VkDeviceMemory block = V.blocks[t.slot / kSlotsPerBlock];
        if (V.BindImageMemory(V.dev, t.img, block, (t.slot % kSlotsPerBlock) * V.slotSize) != VK_SUCCESS) return false;
    }

    memcpy(stage, d.bytes.data(), d.bytes.size());
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    V.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.bufferOffset = stageOff;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { d.w, d.h, 1 };
    V.CmdCopyBufferToImage(cmd, V.stage, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    V.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = d.fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (V.CreateImageView(V.dev, &vi, nullptr, &t.view) != VK_SUCCESS) return false;
    t.ds = ImGui_ImplVulkan_AddTexture(t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!t.ds) return false;
    t.ready = true;
    t.used = g_frame;
    return true;
}

}

void Record(const DeviceInfo& d, VkCommandBuffer cmd, uint32_t frameSlot, uint32_t)
{
    ++g_frame;
    if (!V.init) Init(d);
    if (V.broken) return;
    Evict();

    std::deque<Decoded> batch;
    {
        std::lock_guard lk(g_qmx);
        batch.swap(g_done);
    }
    uint8_t* base = V.stageMap + (frameSlot % kStageSlots) * kStage;
    VkDeviceSize off = (frameSlot % kStageSlots) * kStage, used = 0;
    while (!batch.empty()) {
        Decoded& dec = batch.front();
        if (!dec.ok) { g_tex[dec.path].failed = true; batch.pop_front(); continue; }
        if (used + dec.bytes.size() > kStage) break; // the rest waits for the next frame
        if (!Upload(dec, cmd, base + used, off + used)) {
            Tex& t = g_tex[dec.path];
            Destroy(t);
            t.failed = true;
        }
        used = (used + dec.bytes.size() + 15) & ~15ull;
        batch.pop_front();
    }
    if (!batch.empty()) {
        std::lock_guard lk(g_qmx);
        for (auto it = batch.rbegin(); it != batch.rend(); ++it) g_done.push_front(std::move(*it));
    }
}

void Release()
{
    if (!V.init || V.broken) { V = Vk{}; g_tex.clear(); return; }
    for (auto& [p, t] : g_tex) Destroy(t);
    g_tex.clear();
    for (auto m : V.blocks) V.FreeMemory(V.dev, m, nullptr);
    if (V.stageMap) V.UnmapMemory(V.dev, V.stageMem);
    if (V.stage) V.DestroyBuffer(V.dev, V.stage, nullptr);
    if (V.stageMem) V.FreeMemory(V.dev, V.stageMem, nullptr);
    V = Vk{};
}

uint32_t DescriptorBudget() { return kCap + 200; }

ImTextureID Icon(const std::string& path)
{
    if (path.empty() || V.broken) return ImTextureID{};
    Tex& t = g_tex[path];
    t.used = g_frame;
    if (t.ready) return (ImTextureID)t.ds;
    if (!t.requested && !t.failed && V.init && assets::paks::Ready()) {
        t.requested = true;
        std::lock_guard lk(g_qmx);
        g_requests.push_back(path);
        g_cv.notify_one();
    }
    return ImTextureID{};
}

ImTextureID Rgba(const std::string& key, const unsigned char* rgba, unsigned w, unsigned h)
{
    if (key.empty() || V.broken || (size_t)w * h * 4 > kStage) return ImTextureID{};
    Tex& t = g_tex[key];
    t.used = g_frame;
    if (t.ready) return (ImTextureID)t.ds;
    if (!t.requested && !t.failed && V.init) {
        t.requested = true;
        Decoded d;
        d.path = key;
        d.ok = true;
        d.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        d.w = w;
        d.h = h;
        d.bytes.assign(rgba, rgba + (size_t)w * h * 4);
        std::lock_guard lk(g_qmx);
        g_done.push_back(std::move(d));
    }
    return ImTextureID{};
}

bool Available() { return V.init && !V.broken && assets::paks::Ready(); }

}
