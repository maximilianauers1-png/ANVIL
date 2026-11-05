// Vulkan viewport renderer integrated into Qt Quick's render loop (ADR 0002/0007).
//
// Offscreen pass (MSAA color + reversed-Z depth, resolved) is recorded in
// QQuickWindow::beforeRendering; a fullscreen composite draws it inside Qt's
// render pass (beforeRenderPassRecording), so QML overlays render on top.
// The render thread never copies mesh data: an uploader thread fills
// host-visible buffers; the render thread only records GPU-side copies.
#pragma once
#include <QRectF>
#include <QSize>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "anvil/camera.hpp"

class QQuickWindow;
class QVulkanDeviceFunctions;
class QVulkanFunctions;

namespace anvil {

/// CPU-side mesh prepared by the scene builder (positions relative to `origin`).
struct MeshCpu {
    std::uint64_t key = 0;      // identity (same key = same geometry, GPU buffers reused)
    Vec3 origin;                // world, double
    std::vector<float> pos;     // xyz, float, relative to origin
    std::vector<std::uint32_t> tris;
    std::vector<std::uint32_t> edges;  // feature edges, line list
};

struct DrawItem {
    std::shared_ptr<const MeshCpu> mesh;
    float color[4] = {0.7f, 0.7f, 0.72f, 1.f};
    float highlight = 0.f;      // selection tint 0..1
    Vec3 extra_offset{};        // e.g. move-tool preview
    bool ghost = false;         // translucent, no depth write (previews)
    bool edges = true;
};

struct FrameInput {
    Camera cam;
    std::vector<DrawItem> items;
    QRectF rect_px;             // viewport rect in window device pixels
    float bg[3] = {0.08f, 0.09f, 0.1f};
    float grid[3] = {0.5f, 0.55f, 0.6f};
    float edge[4] = {0.f, 0.f, 0.f, 0.55f};
    float grid_opacity = 0.6f;
    double grid_step = 1.0;
    bool show_grid = true;
};

struct RendererStats {
    std::atomic<double> gpu_ms{0};
    std::atomic<std::uint64_t> triangles{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> gpu_bytes{0};
    std::atomic<std::uint64_t> device_local_budget{0};  // bytes of the largest device-local heap
    std::atomic<bool> ready{false};
    std::string device_name;   // set once at init
    std::uint32_t api_version = 0;
    int msaa = 1;
};

class ViewportRenderer {
  public:
    ViewportRenderer(QQuickWindow* w, std::shared_ptr<RendererStats> stats);
    ~ViewportRenderer();

    void set_input(FrameInput in) { in_ = std::move(in); }  // render thread, during sync
    void before_rendering();        // record offscreen pass (outside Qt's render pass)
    void before_render_pass();      // composite into Qt's render pass

  private:
    struct GpuBuf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        bool device_local = false;
    };
    struct GpuMesh {
        GpuBuf host, dev;           // dev is used if host is not device local
        bool copied = false;
        VkDeviceSize pos_off = 0, tri_off = 0, edge_off = 0;
        std::uint32_t tri_count = 0, edge_count = 0;
        std::uint64_t last_used = 0;
    };
    struct Upload {
        std::shared_ptr<const MeshCpu> mesh;
    };

    void init();
    void create_pipelines();
    void ensure_targets(QSize px);
    void destroy_targets();
    GpuBuf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, bool prefer_device_local);
    void destroy_later(GpuBuf b);
    void destroy_now(GpuBuf& b);
    void upload_loop();
    void collect_finished_uploads(VkCommandBuffer cb);
    VkPipeline make_pipeline(const std::uint32_t* vs, std::size_t vs_size, const std::uint32_t* fs, std::size_t fs_size,
                             VkPrimitiveTopology topo, bool vertex_input, bool blend, bool depth_write,
                             VkCompareOp depth_op, VkRenderPass rp, VkSampleCountFlagBits samples,
                             VkPipelineLayout layout);
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);

    QQuickWindow* window_;
    std::shared_ptr<RendererStats> stats_;
    FrameInput in_;
    bool inited_ = false;

    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    QVulkanDeviceFunctions* df_ = nullptr;
    QVulkanFunctions* f_ = nullptr;
    VkPhysicalDeviceMemoryProperties memprops_{};
    float ts_period_ns_ = 0;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;

    // offscreen targets
    QSize size_;
    VkImage color_ = VK_NULL_HANDLE, depth_ = VK_NULL_HANDLE, resolve_ = VK_NULL_HANDLE;
    VkDeviceMemory color_mem_ = VK_NULL_HANDLE, depth_mem_ = VK_NULL_HANDLE, resolve_mem_ = VK_NULL_HANDLE;
    VkImageView color_view_ = VK_NULL_HANDLE, depth_view_ = VK_NULL_HANDLE, resolve_view_ = VK_NULL_HANDLE;
    VkFramebuffer fb_ = VK_NULL_HANDLE;
    VkRenderPass rp_ = VK_NULL_HANDLE;

    VkPipelineLayout mesh_layout_ = VK_NULL_HANDLE, grid_layout_ = VK_NULL_HANDLE, comp_layout_ = VK_NULL_HANDLE;
    VkPipeline mesh_pipe_ = VK_NULL_HANDLE, ghost_pipe_ = VK_NULL_HANDLE, edge_pipe_ = VK_NULL_HANDLE,
               grid_pipe_ = VK_NULL_HANDLE, comp_pipe_ = VK_NULL_HANDLE;
    VkRenderPass comp_rp_ = VK_NULL_HANDLE;  // Qt's render pass the composite pipeline was built for
    VkDescriptorSetLayout comp_dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool comp_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet comp_set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    bool have_ts_ = false;
    bool drew_offscreen_ = false;

    std::unordered_map<std::uint64_t, GpuMesh> meshes_;
    std::uint64_t frame_ = 0;
    struct Dead {
        std::uint64_t frame;
        GpuBuf b;
        VkImage img = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
    };
    std::vector<Dead> dead_;
    std::vector<std::pair<std::uint64_t, VkDescriptorSet>> retired_sets_;

    // uploader thread
    std::thread uploader_;
    std::mutex up_mu_;
    std::condition_variable up_cv_;
    std::deque<Upload> up_queue_;
    std::vector<std::pair<std::uint64_t, GpuMesh>> up_done_;
    std::unordered_map<std::uint64_t, bool> up_pending_;
    bool up_stop_ = false;
};

}  // namespace anvil
