#include "vk/renderer.hpp"

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QVulkanDeviceFunctions>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "../../render/shaders/generated/shaders_spv.hpp"

namespace anvil {

namespace {
constexpr int kMaxFrameSlots = 4;          // >= Qt's frames in flight (2-3)
constexpr std::uint64_t kEvictFrames = 240; // GPU meshes unused this long are freed
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct MeshPC {
    float viewProj[16];
    float offset[4];
    float color[4];
    float params[4];
};
static_assert(sizeof(MeshPC) == 112);
struct GridPC {
    float invViewProj[16];
    float eye[4];
    float fwd[4];
    float misc[4];
    float color[4];
};
static_assert(sizeof(GridPC) == 128);

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan: ") + what + " failed (" + std::to_string(r) + ")");
}

Mat4 mul(const Mat4& a, const Mat4& b) {  // column-major
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    return r;
}

Mat4 inverse(const Mat4& m) {  // Gauss-Jordan with partial pivoting, double
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) a[r][c] = m[c * 4 + r], a[r][c + 4] = r == c ? 1 : 0;
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        std::swap(a[p], a[c]);
        double d = a[c][c];
        for (double& x : a[c]) x /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    Mat4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = a[r][c + 4];
    return out;
}

void to_float(const Mat4& m, float* out) {
    for (int i = 0; i < 16; ++i) out[i] = float(m[i]);
}

VkDeviceSize align4(VkDeviceSize x) { return (x + 3) & ~VkDeviceSize(3); }
}  // namespace

ViewportRenderer::ViewportRenderer(QQuickWindow* w, std::shared_ptr<RendererStats> stats)
    : window_(w), stats_(std::move(stats)) {}

ViewportRenderer::~ViewportRenderer() {
    {
        std::lock_guard lk(up_mu_);
        up_stop_ = true;
    }
    up_cv_.notify_all();
    if (uploader_.joinable()) uploader_.join();
    if (!inited_) return;
    df_->vkDeviceWaitIdle(dev_);  // teardown only
    for (auto& [k, m] : meshes_) destroy_now(m.host), destroy_now(m.dev);
    for (auto& [k, m] : up_done_) destroy_now(m.host), destroy_now(m.dev);
    for (auto& d : dead_) {
        destroy_now(d.b);
        if (d.fb) df_->vkDestroyFramebuffer(dev_, d.fb, nullptr);
        if (d.view) df_->vkDestroyImageView(dev_, d.view, nullptr);
        if (d.img) df_->vkDestroyImage(dev_, d.img, nullptr);
        if (d.mem) df_->vkFreeMemory(dev_, d.mem, nullptr);
    }
    destroy_targets();
    for (VkPipeline p : {mesh_pipe_, ghost_pipe_, edge_pipe_, grid_pipe_, comp_pipe_})
        if (p) df_->vkDestroyPipeline(dev_, p, nullptr);
    for (VkPipelineLayout l : {mesh_layout_, grid_layout_, comp_layout_})
        if (l) df_->vkDestroyPipelineLayout(dev_, l, nullptr);
    if (comp_pool_) df_->vkDestroyDescriptorPool(dev_, comp_pool_, nullptr);
    if (comp_dsl_) df_->vkDestroyDescriptorSetLayout(dev_, comp_dsl_, nullptr);
    if (sampler_) df_->vkDestroySampler(dev_, sampler_, nullptr);
    if (rp_) df_->vkDestroyRenderPass(dev_, rp_, nullptr);
    if (queries_) df_->vkDestroyQueryPool(dev_, queries_, nullptr);
}

std::uint32_t ViewportRenderer::memory_type(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (std::uint32_t i = 0; i < memprops_.memoryTypeCount; ++i) {
        auto fl = memprops_.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (fl & want) == want && !(fl & avoid)) return i;
    }
    for (std::uint32_t i = 0; i < memprops_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

void ViewportRenderer::init() {
    QSGRendererInterface* rif = window_->rendererInterface();
    auto* inst = static_cast<QVulkanInstance*>(rif->getResource(window_, QSGRendererInterface::VulkanInstanceResource));
    phys_ = *static_cast<VkPhysicalDevice*>(rif->getResource(window_, QSGRendererInterface::PhysicalDeviceResource));
    dev_ = *static_cast<VkDevice*>(rif->getResource(window_, QSGRendererInterface::DeviceResource));
    f_ = inst->functions();
    df_ = inst->deviceFunctions(dev_);
    f_->vkGetPhysicalDeviceMemoryProperties(phys_, &memprops_);
    VkPhysicalDeviceProperties props;
    f_->vkGetPhysicalDeviceProperties(phys_, &props);
    stats_->device_name = props.deviceName;
    stats_->api_version = props.apiVersion;
    VkDeviceSize best = 0;
    for (std::uint32_t i = 0; i < memprops_.memoryHeapCount; ++i)
        if (memprops_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            best = std::max(best, memprops_.memoryHeaps[i].size);
    stats_->device_local_budget = best;

    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples_ = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
               : (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
                                                 : VK_SAMPLE_COUNT_1_BIT;
    stats_->msaa = int(samples_);
    have_ts_ = props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0;
    ts_period_ns_ = props.limits.timestampPeriod;

    // Offscreen render pass: MSAA color + depth, resolved into a sampled image.
    VkAttachmentDescription att[3]{};
    att[0] = {0, kColorFormat, samples_, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    att[1] = {0, kDepthFormat, samples_, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    att[2] = {0, kColorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    if (!msaa) {  // single-sample: render straight into the sampled image
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    sub.pResolveAttachments = msaa ? &rref : nullptr;
    VkSubpassDependency deps[2]{};
    // previous frame's composite read -> this frame's writes (WAR), and depth reuse
    deps[0] = {VK_SUBPASS_EXTERNAL, 0,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
               0};
    deps[1] = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_ACCESS_SHADER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = msaa ? 3 : 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    check(df_->vkCreateRenderPass(dev_, &rpi, nullptr, &rp_), "vkCreateRenderPass");

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;  // 1:1 pixel copy
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(df_->vkCreateSampler(dev_, &si, nullptr, &sampler_), "vkCreateSampler");
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &b;
    check(df_->vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &comp_dsl_), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    check(df_->vkCreateDescriptorPool(dev_, &pi, nullptr, &comp_pool_), "vkCreateDescriptorPool");

    if (have_ts_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kMaxFrameSlots;
        if (df_->vkCreateQueryPool(dev_, &qi, nullptr, &queries_) != VK_SUCCESS) have_ts_ = false;
    }
    create_pipelines();
    uploader_ = std::thread([this] { upload_loop(); });
    inited_ = true;
    stats_->ready = true;
}

VkPipeline ViewportRenderer::make_pipeline(const std::uint32_t* vs, std::size_t vs_size, const std::uint32_t* fs,
                                           std::size_t fs_size, VkPrimitiveTopology topo, bool vertex_input,
                                           bool blend, bool depth_write, VkCompareOp depth_op, VkRenderPass rp,
                                           VkSampleCountFlagBits samples, VkPipelineLayout layout) {
    VkShaderModule mods[2];
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = i ? fs_size : vs_size;
        ci.pCode = i ? fs : vs;
        check(df_->vkCreateShaderModule(dev_, &ci, nullptr, &mods[i]), "vkCreateShaderModule");
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[1], "main", nullptr};
    VkVertexInputBindingDescription vb{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (vertex_input) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 1;
        vi.pVertexAttributeDescriptions = &va;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // show inverted/open meshes instead of hiding them
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth_op != VK_COMPARE_OP_ALWAYS;
    ds.depthWriteEnable = depth_write;
    ds.depthCompareOp = depth_op;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xf;
    if (blend) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cbs;
    gi.pDynamicState = &dy;
    gi.layout = layout;
    gi.renderPass = rp;
    VkPipeline p;
    VkResult r = df_->vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gi, nullptr, &p);
    df_->vkDestroyShaderModule(dev_, mods[0], nullptr);
    df_->vkDestroyShaderModule(dev_, mods[1], nullptr);
    check(r, "vkCreateGraphicsPipelines");
    return p;
}

void ViewportRenderer::create_pipelines() {
    VkPushConstantRange mpr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPC)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &mpr;
    check(df_->vkCreatePipelineLayout(dev_, &li, nullptr, &mesh_layout_), "layout");
    VkPushConstantRange gpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPC)};
    li.pPushConstantRanges = &gpr;
    check(df_->vkCreatePipelineLayout(dev_, &li, nullptr, &grid_layout_), "layout");
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &comp_dsl_;
    check(df_->vkCreatePipelineLayout(dev_, &ci, nullptr, &comp_layout_), "layout");

    using namespace spv;
    const auto GE = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reversed Z
    mesh_pipe_ = make_pipeline(mesh_vert, sizeof mesh_vert, mesh_frag, sizeof mesh_frag,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false, true, GE, rp_, samples_, mesh_layout_);
    ghost_pipe_ = make_pipeline(mesh_vert, sizeof mesh_vert, mesh_frag, sizeof mesh_frag,
                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, true, false, GE, rp_, samples_, mesh_layout_);
    edge_pipe_ = make_pipeline(edge_vert, sizeof edge_vert, edge_frag, sizeof edge_frag,
                               VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, true, false, GE, rp_, samples_, mesh_layout_);
    grid_pipe_ = make_pipeline(fullscreen_vert, sizeof fullscreen_vert, grid_frag, sizeof grid_frag,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true, false, GE, rp_, samples_, grid_layout_);
}

void ViewportRenderer::destroy_targets() {
    for (VkFramebuffer* f : {&fb_}) if (*f) df_->vkDestroyFramebuffer(dev_, *f, nullptr), *f = VK_NULL_HANDLE;
    for (VkImageView* v : {&color_view_, &depth_view_, &resolve_view_})
        if (*v) df_->vkDestroyImageView(dev_, *v, nullptr), *v = VK_NULL_HANDLE;
    for (VkImage* i : {&color_, &depth_, &resolve_}) if (*i) df_->vkDestroyImage(dev_, *i, nullptr), *i = VK_NULL_HANDLE;
    for (VkDeviceMemory* m : {&color_mem_, &depth_mem_, &resolve_mem_})
        if (*m) df_->vkFreeMemory(dev_, *m, nullptr), *m = VK_NULL_HANDLE;
}

void ViewportRenderer::ensure_targets(QSize px) {
    if (px == size_ && fb_) return;
    // Old targets may still be read by frames in flight: retire them by frame number.
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    if (fb_) {
        dead_.push_back({frame_, {}, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, fb_});
        dead_.push_back({frame_, {}, color_, color_view_, color_mem_, VK_NULL_HANDLE});
        dead_.push_back({frame_, {}, depth_, depth_view_, depth_mem_, VK_NULL_HANDLE});
        if (resolve_) dead_.push_back({frame_, {}, resolve_, resolve_view_, resolve_mem_, VK_NULL_HANDLE});
        fb_ = VK_NULL_HANDLE;
        color_ = depth_ = resolve_ = VK_NULL_HANDLE;
        color_view_ = depth_view_ = resolve_view_ = VK_NULL_HANDLE;
        color_mem_ = depth_mem_ = resolve_mem_ = VK_NULL_HANDLE;
    }
    size_ = px;
    auto make_image = [&](VkFormat fmt, VkSampleCountFlagBits s, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {std::uint32_t(px.width()), std::uint32_t(px.height()), 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = s;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        check(df_->vkCreateImage(dev_, &ii, nullptr, &img), "vkCreateImage");
        VkMemoryRequirements req;
        df_->vkGetImageMemoryRequirements(dev_, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(df_->vkAllocateMemory(dev_, &ai, nullptr, &mem), "vkAllocateMemory(image)");
        df_->vkBindImageMemory(dev_, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        check(df_->vkCreateImageView(dev_, &vi, nullptr, &view), "vkCreateImageView");
    };
    VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (msaa ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT);
    make_image(kColorFormat, samples_, cu, VK_IMAGE_ASPECT_COLOR_BIT, color_, color_mem_, color_view_);
    make_image(kDepthFormat, samples_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth_,
               depth_mem_, depth_view_);
    if (msaa)
        make_image(kColorFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, resolve_, resolve_mem_, resolve_view_);
    VkImageView views[3] = {color_view_, depth_view_, resolve_view_};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp_;
    fi.attachmentCount = msaa ? 3 : 2;
    fi.pAttachments = views;
    fi.width = std::uint32_t(px.width());
    fi.height = std::uint32_t(px.height());
    fi.layers = 1;
    check(df_->vkCreateFramebuffer(dev_, &fi, nullptr, &fb_), "vkCreateFramebuffer");

    // New descriptor set (the old one may still be in use by a frame in flight).
    if (comp_set_) retired_sets_.push_back({frame_, comp_set_});
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = comp_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &comp_dsl_;
    check(df_->vkAllocateDescriptorSets(dev_, &dai, &comp_set_), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo info{sampler_, msaa ? resolve_view_ : color_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = comp_set_;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    df_->vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}

ViewportRenderer::GpuBuf ViewportRenderer::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
                                                       bool prefer_device_local) {
    GpuBuf b;
    b.size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    check(df_->vkCreateBuffer(dev_, &bi, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    df_->vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    std::uint32_t type = UINT32_MAX;
    if (host_visible) {
        auto hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (prefer_device_local) type = memory_type(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) type = memory_type(req.memoryTypeBits, hv);
    } else {
        type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (type == UINT32_MAX) throw std::runtime_error("no suitable Vulkan memory type");
    b.device_local = memprops_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkResult r = df_->vkAllocateMemory(dev_, &ai, nullptr, &b.mem);
    if (r != VK_SUCCESS) {
        df_->vkDestroyBuffer(dev_, b.buf, nullptr);
        throw std::runtime_error("out of GPU memory for a mesh buffer (" + std::to_string(size >> 20) + " MiB)");
    }
    df_->vkBindBufferMemory(dev_, b.buf, b.mem, 0);
    stats_->gpu_bytes += req.size;
    return b;
}

void ViewportRenderer::destroy_now(GpuBuf& b) {
    if (b.buf) df_->vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) {
        df_->vkFreeMemory(dev_, b.mem, nullptr);
        stats_->gpu_bytes -= std::min<std::uint64_t>(stats_->gpu_bytes, b.size);
    }
    b = {};
}

void ViewportRenderer::destroy_later(GpuBuf b) {
    if (b.buf) dead_.push_back({frame_, b});
}

void ViewportRenderer::upload_loop() {
    // Large heaps that are both device local and host visible mean resizable BAR:
    // then the GPU reads the mesh directly; otherwise the render thread records a copy.
    VkDeviceSize rebar = 0;
    for (std::uint32_t i = 0; i < memprops_.memoryTypeCount; ++i) {
        auto fl = memprops_.memoryTypes[i].propertyFlags;
        if ((fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            rebar = std::max(rebar, memprops_.memoryHeaps[memprops_.memoryTypes[i].heapIndex].size);
    }
    const bool prefer_bar = rebar >= (VkDeviceSize(1) << 30);
    for (;;) {
        Upload u;
        {
            std::unique_lock lk(up_mu_);
            up_cv_.wait(lk, [&] { return up_stop_ || !up_queue_.empty(); });
            if (up_stop_) return;
            u = std::move(up_queue_.front());
            up_queue_.pop_front();
        }
        const MeshCpu& m = *u.mesh;
        GpuMesh g;
        g.pos_off = 0;
        g.tri_off = align4(m.pos.size() * 4);
        g.edge_off = g.tri_off + align4(m.tris.size() * 4);
        VkDeviceSize total = std::max<VkDeviceSize>(g.edge_off + m.edges.size() * 4, 16);
        g.tri_count = std::uint32_t(m.tris.size());
        g.edge_count = std::uint32_t(m.edges.size());
        try {
            g.host = make_buffer(total,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 true, prefer_bar);
            void* p = nullptr;
            check(df_->vkMapMemory(dev_, g.host.mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
            auto* bytes = static_cast<char*>(p);
            std::memcpy(bytes + g.pos_off, m.pos.data(), m.pos.size() * 4);
            std::memcpy(bytes + g.tri_off, m.tris.data(), m.tris.size() * 4);
            std::memcpy(bytes + g.edge_off, m.edges.data(), m.edges.size() * 4);
            df_->vkUnmapMemory(dev_, g.host.mem);
        } catch (const std::exception&) {
            destroy_now(g.host);
        }
        std::lock_guard lk(up_mu_);
        up_done_.emplace_back(m.key, g);
    }
}

void ViewportRenderer::collect_finished_uploads(VkCommandBuffer cb) {
    std::vector<std::pair<std::uint64_t, GpuMesh>> done;
    {
        std::unique_lock lk(up_mu_, std::try_to_lock);  // never wait on the uploader
        if (!lk.owns_lock()) return;
        done.swap(up_done_);
        for (auto& d : done) up_pending_.erase(d.first);
    }
    for (auto& [key, g] : done) {
        if (!g.host.buf) continue;  // allocation failed; will be retried on next request
        if (!g.host.device_local) {
            g.dev = make_buffer(g.host.size,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, true);
            VkBufferCopy c{0, 0, g.host.size};
            df_->vkCmdCopyBuffer(cb, g.host.buf, g.dev.buf, 1, &c);
            VkBufferMemoryBarrier bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.buffer = g.dev.buf;
            bar.size = VK_WHOLE_SIZE;
            df_->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0,
                                      nullptr, 1, &bar, 0, nullptr);
            destroy_later(g.host);
            g.host = {};
        }
        auto it = meshes_.find(key);
        if (it != meshes_.end()) destroy_later(it->second.host), destroy_later(it->second.dev);
        g.last_used = frame_;
        meshes_[key] = g;
    }
}

void ViewportRenderer::before_rendering() {
    if (!inited_) init();
    ++frame_;
    drew_offscreen_ = false;
    const int fif = std::max(1, window_->graphicsStateInfo().framesInFlight);
    // retire resources no frame in flight can still use
    std::erase_if(dead_, [&](Dead& d) {
        if (d.frame + std::uint64_t(fif) + 1 > frame_) return false;
        destroy_now(d.b);
        if (d.fb) df_->vkDestroyFramebuffer(dev_, d.fb, nullptr);
        if (d.view) df_->vkDestroyImageView(dev_, d.view, nullptr);
        if (d.img) df_->vkDestroyImage(dev_, d.img, nullptr);
        if (d.mem) df_->vkFreeMemory(dev_, d.mem, nullptr);
        return true;
    });
    std::erase_if(retired_sets_, [&](auto& s) {
        if (s.first + std::uint64_t(fif) + 1 > frame_) return false;
        df_->vkFreeDescriptorSets(dev_, comp_pool_, 1, &s.second);
        return true;
    });

    QSize px(int(in_.rect_px.width()), int(in_.rect_px.height()));
    if (px.width() < 1 || px.height() < 1) return;

    QSGRendererInterface* rif = window_->rendererInterface();
    VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(rif->getResource(window_, QSGRendererInterface::CommandListResource));
    window_->beginExternalCommands();
    collect_finished_uploads(cb);

    // request uploads for meshes we do not have yet; evict unused ones
    std::uint64_t tris = 0;
    {
        std::lock_guard lk(up_mu_);
        for (const auto& it : in_.items) {
            if (!it.mesh) continue;
            auto m = meshes_.find(it.mesh->key);
            if (m != meshes_.end()) {
                m->second.last_used = frame_;
                continue;
            }
            if (!up_pending_.count(it.mesh->key)) {
                up_pending_[it.mesh->key] = true;
                up_queue_.push_back({it.mesh});
            }
        }
    }
    up_cv_.notify_one();
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (frame_ - it->second.last_used > kEvictFrames) {
            destroy_later(it->second.host);
            destroy_later(it->second.dev);
            it = meshes_.erase(it);
        } else ++it;
    }

    ensure_targets(px);
    const int slot = int(frame_ % kMaxFrameSlots);
    if (have_ts_) {
        std::uint64_t ts[2];
        if (frame_ > kMaxFrameSlots &&
            df_->vkGetQueryPoolResults(dev_, queries_, 2 * slot, 2, sizeof ts, ts, sizeof(std::uint64_t),
                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            stats_->gpu_ms = double(ts[1] - ts[0]) * ts_period_ns_ * 1e-6;
        df_->vkCmdResetQueryPool(cb, queries_, 2 * slot, 2);
        df_->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 2 * slot);
    }

    VkClearValue clear[3]{};
    clear[0].color = {{in_.bg[0], in_.bg[1], in_.bg[2], 1.f}};
    clear[1].depthStencil = {0.f, 0};  // reversed Z: far = 0
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp_;
    bi.framebuffer = fb_;
    bi.renderArea = {{0, 0}, {std::uint32_t(px.width()), std::uint32_t(px.height())}};
    bi.clearValueCount = 3;
    bi.pClearValues = clear;
    df_->vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, float(px.width()), float(px.height()), 0.f, 1.f};
    VkRect2D sc{{0, 0}, {std::uint32_t(px.width()), std::uint32_t(px.height())}};
    df_->vkCmdSetViewport(cb, 0, 1, &vp);
    df_->vkCmdSetScissor(cb, 0, 1, &sc);

    Camera cam = in_.cam;
    cam.width = px.width();
    cam.height = px.height();
    const Mat4 P = cam.projection(), V = cam.view_rotation(), VP = mul(P, V);
    const Vec3 eye = cam.eye();

    auto draw = [&](const DrawItem& it, VkPipeline pipe, bool edges) {
        auto m = meshes_.find(it.mesh->key);
        if (m == meshes_.end()) return;
        const GpuMesh& g = m->second;
        if (edges && !g.edge_count) return;
        VkBuffer buf = g.dev.buf ? g.dev.buf : g.host.buf;
        MeshPC pc{};
        to_float(VP, pc.viewProj);
        Vec3 off = it.mesh->origin + it.extra_offset - eye;  // double, then float (camera-relative)
        pc.offset[0] = float(off.x), pc.offset[1] = float(off.y), pc.offset[2] = float(off.z);
        if (edges) {
            std::memcpy(pc.color, in_.edge, sizeof pc.color);
            // ~2 pixels of depth bias (see edge.vert)
            const double px_angle = 2.0 * std::tan(cam.fov_y / 2) / std::max(cam.height, 1.0);
            pc.params[1] = cam.ortho ? 0.f : float(2.0 * px_angle);
            pc.params[2] = cam.ortho ? float(2.0 * cam.world_per_pixel(cam.distance) / (2 * cam.ortho_range())) : 0.f;
        } else {
            std::memcpy(pc.color, it.color, sizeof pc.color);
        }
        pc.params[0] = it.highlight;
        df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        df_->vkCmdPushConstants(cb, mesh_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof pc, &pc);
        VkDeviceSize vo = g.pos_off;
        df_->vkCmdBindVertexBuffers(cb, 0, 1, &buf, &vo);
        df_->vkCmdBindIndexBuffer(cb, buf, edges ? g.edge_off : g.tri_off, VK_INDEX_TYPE_UINT32);
        df_->vkCmdDrawIndexed(cb, edges ? g.edge_count : g.tri_count, 1, 0, 0, 0);
        if (!edges) tris += g.tri_count / 3;
    };
    for (const auto& it : in_.items)
        if (it.mesh && !it.ghost) draw(it, mesh_pipe_, false);
    for (const auto& it : in_.items)
        if (it.mesh && !it.ghost && it.edges) draw(it, edge_pipe_, true);
    if (in_.show_grid) {
        GridPC g{};
        to_float(inverse(VP), g.invViewProj);
        g.eye[0] = float(eye.x), g.eye[1] = float(eye.y), g.eye[2] = float(eye.z), g.eye[3] = float(in_.grid_step);
        Vec3 f = cam.forward();
        g.fwd[0] = float(f.x), g.fwd[1] = float(f.y), g.fwd[2] = float(f.z), g.fwd[3] = float(cam.near_plane());
        g.misc[0] = cam.ortho ? 1.f : 0.f;
        g.misc[1] = float(cam.ortho_range()), g.misc[2] = float(-cam.ortho_range());  // Camera's ortho slab
        g.misc[3] = in_.grid_opacity;
        g.color[0] = in_.grid[0], g.color[1] = in_.grid[1], g.color[2] = in_.grid[2], g.color[3] = 1.f;
        df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipe_);
        df_->vkCmdPushConstants(cb, grid_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g, &g);
        df_->vkCmdDraw(cb, 3, 1, 0, 0);
    }
    for (const auto& it : in_.items)
        if (it.mesh && it.ghost) draw(it, ghost_pipe_, false);
    df_->vkCmdEndRenderPass(cb);
    if (have_ts_) df_->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2 * slot + 1);
    window_->endExternalCommands();
    stats_->triangles = tris;
    ++stats_->frames;
    drew_offscreen_ = true;
}

void ViewportRenderer::before_render_pass() {
    if (!drew_offscreen_) return;
    QSGRendererInterface* rif = window_->rendererInterface();
    VkRenderPass rp = *static_cast<VkRenderPass*>(rif->getResource(window_, QSGRendererInterface::RenderPassResource));
    if (rp != comp_rp_) {
        if (comp_pipe_) df_->vkDestroyPipeline(dev_, comp_pipe_, nullptr);  // only on render pass change
        comp_pipe_ = make_pipeline(spv::fullscreen_vert, sizeof spv::fullscreen_vert, spv::composite_frag,
                                   sizeof spv::composite_frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, false, false,
                                   VK_COMPARE_OP_ALWAYS, rp, VK_SAMPLE_COUNT_1_BIT, comp_layout_);
        comp_rp_ = rp;
    }
    VkCommandBuffer cb = *static_cast<VkCommandBuffer*>(rif->getResource(window_, QSGRendererInterface::CommandListResource));
    window_->beginExternalCommands();
    const QRectF& r = in_.rect_px;
    VkViewport vp{float(r.x()), float(r.y()), float(r.width()), float(r.height()), 0.f, 1.f};
    VkRect2D sc{{int(r.x()), int(r.y())}, {std::uint32_t(r.width()), std::uint32_t(r.height())}};
    df_->vkCmdSetViewport(cb, 0, 1, &vp);
    df_->vkCmdSetScissor(cb, 0, 1, &sc);
    df_->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, comp_pipe_);
    df_->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, comp_layout_, 0, 1, &comp_set_, 0, nullptr);
    df_->vkCmdDraw(cb, 3, 1, 0, 0);
    window_->endExternalCommands();
}

}  // namespace anvil
