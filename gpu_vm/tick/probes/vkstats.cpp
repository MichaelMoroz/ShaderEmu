// vkstats: the driver's statistics (and internal representations, if it gives any) of a pixel shader's pipeline, through
// VK_KHR_pipeline_executable_properties: vkstats VS.spv PS.spv [OUT.txt]. The shader is a tick (8 uint4 targets,
// textures at bindings 100-111, cbuffers at 200-201: dxc -spirv -fvk-t-shift 100 0 -fvk-b-shift 200 0).
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<char> slurp(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) { printf("no %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> b(n); fread(b.data(), 1, n, f); fclose(f); return b;
}
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s: %d\n", #x, (int)r_); exit(1); } } while (0)
int main(int argc, char** argv) {
    if (argc < 3) { printf("vkstats VS.spv PS.spv [OUT]\n"); return 1; }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    VkInstance inst; CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (auto d : pds) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p); printf("device: %s\n", p.deviceName); if (p.vendorID == 0x10de) pd = d; }
    if (!pd) { printf("no NVIDIA device\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount = 1; qci.pQueuePriorities = &prio; qci.queueFamilyIndex = 0;
    const char* ext[] = {"VK_KHR_pipeline_executable_properties"};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    pef.pipelineExecutableInfo = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &pef;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = ext;
    VkDevice dev; CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    auto getProps = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
    auto getStats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");
    auto getIR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
    auto mod = [&](const char* p) {
        std::vector<char> c = slurp(p);
        VkShaderModuleCreateInfo m = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; m.codeSize = c.size(); m.pCode = (const uint32_t*)c.data();
        VkShaderModule s; CK(vkCreateShaderModule(dev, &m, nullptr, &s)); return s;
    };
    VkShaderModule vs = mod(argv[1]), ps = mod(argv[2]);
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < 13; i++) b.push_back({100 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    for (uint32_t i = 0; i < 2; i++) b.push_back({200 + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = (uint32_t)b.size(); dl.pBindings = b.data();
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl));
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &dsl;
    VkPipelineLayout layout; CK(vkCreatePipelineLayout(dev, &pl, nullptr, &layout));
    const int NT = 8;
    std::vector<VkAttachmentDescription> att(NT);
    std::vector<VkAttachmentReference> ref(NT);
    for (int i = 0; i < NT; i++) {
        att[i] = {0, VK_FORMAT_R32G32B32A32_UINT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
        ref[i] = {(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = NT; sp.pColorAttachments = ref.data();
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount = NT; rp.pAttachments = att.data(); rp.subpassCount = 1; rp.pSubpasses = &sp;
    VkRenderPass pass; CK(vkCreateRenderPass(dev, &rp, nullptr, &pass));
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = argc > 4 ? argv[4] : "vert";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = ps; st[1].pName = argc > 5 ? argv[5] : "frag";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, 38, 1, 0, 1}; VkRect2D sc = {{0, 0}, {38, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> cb(NT);
    for (auto& c : cb) { c = {}; c.colorWriteMask = 15; }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cbs.attachmentCount = NT; cbs.pAttachments = cb.data();
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cbs; gp.layout = layout; gp.renderPass = pass;
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe));
    VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR}; pi.pipeline = pipe;
    uint32_t ne = 0; CK(getProps(dev, &pi, &ne, nullptr));
    std::vector<VkPipelineExecutablePropertiesKHR> props(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    CK(getProps(dev, &pi, &ne, props.data()));
    FILE* out = argc > 3 ? fopen(argv[3], "w") : nullptr;
    for (uint32_t e = 0; e < ne; e++) {
        printf("== %s: %s (subgroup %u)\n", props[e].name, props[e].description, props[e].subgroupSize);
        VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR}; ei.pipeline = pipe; ei.executableIndex = e;
        uint32_t ns = 0; CK(getStats(dev, &ei, &ns, nullptr));
        std::vector<VkPipelineExecutableStatisticKHR> s(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        CK(getStats(dev, &ei, &ns, s.data()));
        for (auto& x : s) {
            printf("  %s: ", x.name);
            switch (x.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: printf("%u", x.value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: printf("%lld", (long long)x.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: printf("%llu", (unsigned long long)x.value.u64); break;
            default: printf("%g", x.value.f64);
            }
            printf("  (%s)\n", x.description);
        }
        uint32_t nr = 0;
        if (getIR(dev, &ei, &nr, nullptr) == VK_SUCCESS && nr) {
            std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            getIR(dev, &ei, &nr, ir.data());
            std::vector<std::vector<char>> data(nr);
            for (uint32_t i = 0; i < nr; i++) { data[i].resize(ir[i].dataSize + 1); ir[i].pData = data[i].data(); }
            getIR(dev, &ei, &nr, ir.data());
            for (uint32_t i = 0; i < nr; i++) {
                printf("  representation %s (%s): %zu bytes%s\n", ir[i].name, ir[i].description, ir[i].dataSize, ir[i].isText ? ", text" : "");
                if (out && ir[i].isText) fprintf(out, "==== %s %s\n%s\n", props[e].name, ir[i].name, data[i].data());
            }
        } else printf("  no internal representations\n");
    }
    if (out) fclose(out);
    return 0;
}
