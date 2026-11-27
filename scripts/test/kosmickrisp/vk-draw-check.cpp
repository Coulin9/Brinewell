#include <vulkan/vulkan.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const uint32_t vertexCode[] =
#include "triangle.vert.inc"
;
static const uint32_t fragmentCode[] =
#include "color.frag.inc"
;
static const uint32_t faceVertexCode[] =
#include "faces.vert.inc"
;
static const uint32_t faceFragmentCode[] =
#include "faces.frag.inc"
;
static const uint32_t primitiveIDFragmentCode[] =
#include "primitive-id.frag.inc"
;
static const uint32_t primitiveIDGeometryCode[] =
#include "primitive-id.geom.inc"
;
static const uint32_t depthSlopeCode[] =
#include "depth-slope.vert.inc"
;
static const uint32_t duplicateCode[] =
#include "duplicate.geom.inc"
;
static const uint32_t conditionalCode[] =
#include "conditional.geom.inc"
;
static const uint32_t storeCode[] =
#include "store.geom.inc"
;

constexpr uint32_t side = 64;
constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t storePoints = 67, storeGuards = 2;
constexpr size_t storeBytes = (storePoints + storeGuards) * 4 * sizeof(uint32_t);
constexpr size_t indexOffset = 1280, argsOffset = 1344, imageOffset = 1536;
static_assert(storeBytes <= indexOffset);
constexpr size_t byteCount = imageOffset + side * side * 4;

static void fail(const char *s) {
    std::fprintf(stderr, "%s\n", s);
    std::exit(2);
}
static void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s returned %d\n", operation, result);
        std::exit(2);
    }
}
#define VK_CHECK(call) check((call), #call)

enum class Case { solid, line, point, lineCull, pointCull, indexedPoint, geometry, geometryPoint, conditional, store, faces, primitiveID, geometryID, depthSlope, depthControl };
enum class FaceTest { both, cullBack, stencil, nearClip };
struct Test {
    Case id;
    const char *name;
    VkPolygonMode mode = VK_POLYGON_MODE_FILL;
    FaceTest face = FaceTest::both;
};
constexpr Test tests[] = {
    {Case::solid, "solid"}, {Case::line, "line"}, {Case::point, "point"},
    {Case::lineCull, "line-cull"}, {Case::pointCull, "point-cull"},
    {Case::indexedPoint, "point-indexed-indirect"}, {Case::geometry, "geometry"},
    {Case::geometryPoint, "geometry-point"}, {Case::conditional, "geometry-indirect"},
    {Case::store, "geometry-store"},
    {Case::faces, "fill-face", VK_POLYGON_MODE_FILL},
    {Case::faces, "line-face", VK_POLYGON_MODE_LINE},
    {Case::faces, "point-face", VK_POLYGON_MODE_POINT},
    {Case::faces, "line-back-cull", VK_POLYGON_MODE_LINE, FaceTest::cullBack},
    {Case::faces, "point-back-cull", VK_POLYGON_MODE_POINT, FaceTest::cullBack},
    {Case::faces, "fill-stencil", VK_POLYGON_MODE_FILL, FaceTest::stencil},
    {Case::faces, "point-stencil", VK_POLYGON_MODE_POINT, FaceTest::stencil},
    {Case::faces, "fill-clip", VK_POLYGON_MODE_FILL, FaceTest::nearClip},
    {Case::faces, "line-clip", VK_POLYGON_MODE_LINE, FaceTest::nearClip},
    {Case::faces, "point-clip", VK_POLYGON_MODE_POINT, FaceTest::nearClip},
    {Case::primitiveID, "fill-primitive-id", VK_POLYGON_MODE_FILL},
    {Case::primitiveID, "point-primitive-id", VK_POLYGON_MODE_POINT},
    {Case::geometryID, "geometry-fill-primitive-id", VK_POLYGON_MODE_FILL},
    {Case::geometryID, "geometry-point-primitive-id", VK_POLYGON_MODE_POINT},
    {Case::depthControl, "point-depth-control", VK_POLYGON_MODE_POINT},
    {Case::depthSlope, "fill-depth-bias", VK_POLYGON_MODE_FILL},
    {Case::depthSlope, "point-depth-bias", VK_POLYGON_MODE_POINT},
};

struct Pixel { uint8_t r, g, b, a; };
enum Color { black, red, green, invalid };
static Color color(Pixel p) {
    if (p.a != 255 || p.b) return invalid;
    if (!p.r && !p.g) return black;
    if (p.r == 255 && !p.g) return red;
    if (!p.r && p.g == 255) return green;
    return invalid;
}

// Signed distance to the three edge lines of this right isosceles triangle.
// Ignore a narrow edge band for filled triangles to allow rounding differences.
static float inside(float x, float y, float left, float top, float size) {
    return std::min({x - left, y - top, (size - (x - left) - (y - top)) * 0.70710678f});
}

static bool checkTriangles(Color got, float redDistance, float greenDistance) {
    if (redDistance > 1) return got == red;
    if (greenDistance > 1) return got == green;
    if (got == red) return redDistance >= -1;
    if (got == green) return greenDistance >= -1;
    return got == black;
}

static bool conditionalPixel(Color got, float x, float y) {
    // The index buffer fetches vertices 6, 4, 5, distributed over columns 2, 0, 1.
    // Each input point launches two GS invocations per instance and emits 0–2 strips.
    float rd = -1000, gd = -1000;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 3; ++column) {
            int primitive = (column + 1) % 3;
            int seed = primitive + row % 2 + row / 2;
            if (seed % 3 == 0) continue;
            float left = 3.25f + column * 20, top = 4.25f + row * 14;
            rd = std::max(rd, inside(x, y, left, top, 6));
            if (seed & 1)
                gd = std::max(gd, inside(x, y, left + 8, top, 6));
        }
    }
    return checkTriangles(got, rd, gd);
}

static bool facePixel(const Test &test, Color got, int x, int y) {
    bool front = x < 32;
    Color face = front ? red : green;
    if ((test.face == FaceTest::stencil && front) || (test.face == FaceTest::cullBack && !front))
        return got == black;
    bool clip = test.face == FaceTest::nearClip;
    int u = x - (front ? 6 : 36), v = y - 10;
    if (test.mode == VK_POLYGON_MODE_POINT) {
        bool survivingPoint = (u == 0 && v == 20) || (u == 20 && v == 0);
        if (survivingPoint || (!clip && u == 0 && v == 0)) return got == face;
        // At the near-plane intersections permit either coverage result. Check
        // the surviving corners, rejected corner and empty polygon interior exactly.
        if (clip && ((u == 0 && v == 10) || (u == 10 && v == 0))) return got == face || got == black;
        return got == black;
    }
    float d = inside(u + 0.5f, v + 0.5f, .25f, .25f, 20);
    if (clip) d = std::min(d, (u + v - 9.5f) * .70710678f);
    if (test.mode == VK_POLYGON_MODE_FILL) {
        if (d > 1) return got == face;
        if (d < -1) return got == black;
        return got == face || got == black;
    }
    return got == black || (got == face && std::abs(d) < 1.5f && u >= -1 && u <= 21 && v >= -1 && v <= 21);
}

static bool checkReadback(const Test &test, const uint8_t *bytes) {
    const auto *image = reinterpret_cast<const Pixel *>(bytes + imageOffset);
    unsigned count = 0, errors = 0;
    auto mismatch = [&](int x, int y) {
        if (!errors) {
            Pixel p = image[y * side + x];
            std::fprintf(stderr, "%s: first unexpected pixel at (%d,%d), RGBA=(%u,%u,%u,%u)\n",
                         test.name, x, y, p.r, p.g, p.b, p.a);
        }
        ++errors;
    };
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            const Color got = color(image[y * side + x]);
            const float px = x + 0.5f, py = y + 0.5f;
            const float dist = inside(px, py, 8.25f, 8.25f, 48);
            bool good = true;
            if (got != black) ++count;
            switch (test.id) {
            case Case::solid:
                good = got != green && (dist > 1 ? got == red : (dist >= -1 || got == black));
                break;
            case Case::line:
                good = got == black || (got == red && std::abs(dist) < 1.5f &&
                                         px >= 7 && py >= 7 && px <= 58 && py <= 58);
                break;
            case Case::point:
            case Case::indexedPoint: {
                const int offsetX = test.id == Case::indexedPoint ? 3 : 0;
                const int offsetY = test.id == Case::indexedPoint ? 5 : 0;
                const int i = static_cast<int>(x) - offsetX, j = static_cast<int>(y) - offsetY;
                bool vertex = (i == 8 && j == 8) || (i == 56 && j == 8) || (i == 8 && j == 56);
                good = got == (vertex ? red : black);
                break;
            }
            case Case::geometry: {
                float left = inside(px, py, 8.3275f, 23.6875f, 16.8f);
                float right = inside(px, py, 39.0475f, 23.6875f, 16.8f);
                good = checkTriangles(got, left, right);
                break;
            }
            case Case::geometryPoint: {
                Color expected = black;
                if ((x == 8 && (y == 23 || y == 40)) || (x == 25 && y == 23)) expected = red;
                if ((x == 39 && (y == 23 || y == 40)) || (x == 55 && y == 23)) expected = green;
                good = got == expected;
                break;
            }
            case Case::conditional:
                good = conditionalPixel(got, px, py);
                break;
            case Case::faces:
            case Case::primitiveID:
            case Case::geometryID:
                good = facePixel(test, got, x, y);
                break;
            case Case::depthSlope:
            case Case::depthControl: {
                bool left = x < 32;
                bool rejected = left && test.id == Case::depthSlope;
                if (rejected) good = got == black;
                else if (test.mode == VK_POLYGON_MODE_POINT) {
                    unsigned base = left ? 6 : 36;
                    bool vertex = (x == base && (y == 10 || y == 30)) || (!left && x == base + 20 && y == 10);
                    good = got == (vertex ? red : black);
                } else {
                    good = checkTriangles(got, inside(px, py, 36.25f, 10.25f, 20), -1000);
                }
                break;
            }
            case Case::lineCull:
            case Case::pointCull:
            case Case::store:
                good = got == black;
                break;
            }
            if (!good || got == invalid) mismatch(x, y);
        }
    }
    if (errors && test.id == Case::faces && test.mode == VK_POLYGON_MODE_POINT) {
        unsigned wrongFlat = 0, wrongFacing = 0;
        for (size_t i = 0; i < side * side; ++i) {
            Pixel p = image[i];
            if (!p.r && !p.g && p.b) ++wrongFlat;
            if (p.r && (p.g || p.b)) ++wrongFacing;
        }
        std::fprintf(stderr, "%s: non-provoking tag=%u, wrong facing with correct tag=%u\n",
                     test.name, wrongFlat, wrongFacing);
    }
    if (test.id == Case::faces && test.mode == VK_POLYGON_MODE_LINE) {
        for (const int left : {6, 36}) {
            if (left == 36 && test.face == FaceTest::cullBack) continue;
            // All three original edge segments survive even when corner 0 is clipped.
            const std::array<std::array<int, 2>, 3> samples = {{{left + 15, 10}, {left, 25}, {left + 10, 20}}};
            for (auto [x, y] : samples) {
                bool found = false;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        found |= color(image[(y + j) * side + x + i]) == (left == 6 ? red : green);
                if (!found) mismatch(x, y);
            }
        }
    }
    if (test.id == Case::line) {
        // Sample the full length of all three edges, tolerating line-end rules.
        for (int step = 4; step <= 44; ++step) {
            const std::array<std::array<int, 2>, 3> samples = {{{8 + step, 8}, {8, 8 + step}, {56 - step, 8 + step}}};
            for (auto [x, y] : samples) {
                bool found = false;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i)
                        found |= color(image[(y + j) * side + x + i]) == red;
                if (!found) mismatch(x, y);
            }
        }
    }
    if (test.id == Case::store) {
        const unsigned first[7][2] = {{8,8}, {56,8}, {8,56}, {8,56}, {11,13}, {59,13}, {11,61}};
        for (unsigned id = 0; id < storePoints + storeGuards; ++id) {
            uint32_t expected[] = {0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu};
            if (id < storePoints) {
                ++expected[0]; // atomicAdd must execute exactly once per input primitive.
                expected[1] = id < 7 ? first[id][0] : 2 + id % 42;
                expected[2] = id < 7 ? first[id][1] : 3 + id / 42;
                expected[3] = 0xabcd0123u ^ id;
            }
            if (std::memcmp(bytes + id * sizeof(expected), expected, sizeof(expected)) != 0) {
                if (!errors) std::fprintf(stderr, "%s: first unexpected storage record %u\n", test.name, id);
                ++errors;
            }
        }
    }
    std::printf("%s %s  covered=%u  errors=%u\n", errors ? "FAIL" : "PASS", test.name, count, errors);
    return !errors;
}

struct Fixture {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkPhysicalDeviceFeatures features{};
    VkDevice device{};
    VkQueue queue{};
    VkImage target{};
    VkImageView targetView{};
    VkDeviceMemory targetMemory{}, bufferMemory{};
    VkImage stencil{};
    VkImageView stencilView{};
    VkDeviceMemory stencilMemory{};
    VkImage depth{};
    VkImageView depthView{};
    VkDeviceMemory depthMemory{};
    VkBuffer buffer{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptorSet{};
    VkCommandPool commandPool{};
    VkCommandBuffer cmd{};
    uint8_t *mapped{};

    VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(gpu, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
                VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                info.allocationSize = req.size;
                info.memoryTypeIndex = i;
                VkDeviceMemory memory;
                VK_CHECK(vkAllocateMemory(device, &info, nullptr, &memory));
                return memory;
            }
        }
        fail("no suitable Vulkan memory type");
        return VK_NULL_HANDLE;
    }

    void createAttachment(VkFormat pixelFormat, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          VkImage &image, VkImageView &view, VkDeviceMemory &memory) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D; ic.format = pixelFormat; ic.extent = {side, side, 1};
        ic.mipLevels = ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.usage = usage;
        VK_CHECK(vkCreateImage(device, &ic, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image, &req);
        memory = allocate(req, 0);
        VK_CHECK(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = image; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = pixelFormat;
        iv.subresourceRange = {aspect, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &iv, nullptr, &view));
    }

    explicit Fixture(const char *filter) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Brinewell Vulkan draw check";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo creation{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        creation.pApplicationInfo = &app;
#ifdef __APPLE__
        const char *portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        creation.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        creation.enabledExtensionCount = 1;
        creation.ppEnabledExtensionNames = &portability;
#endif
        VK_CHECK(vkCreateInstance(&creation, nullptr, &instance));
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> gpus(count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));
        for (VkPhysicalDevice candidate : gpus) {
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
            vkGetPhysicalDeviceProperties2(candidate, &props);
            if (!std::strstr(props.properties.deviceName, filter) && !std::strstr(driver.driverName, filter)) continue;
            if (gpu) fail("device filter matches multiple devices");
            gpu = candidate;
            std::printf("pid=%ld device=%s driver=%s (%s)\n", static_cast<long>(getpid()),
                        props.properties.deviceName, driver.driverName, driver.driverInfo);
        }
        if (!gpu) fail("no matching Vulkan device");
        vkGetPhysicalDeviceFeatures(gpu, &features);
        VkPhysicalDeviceFeatures enabled{};
        enabled.fillModeNonSolid = features.fillModeNonSolid;
        enabled.geometryShader = features.geometryShader;
        enabled.vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics;
        std::printf("fillModeNonSolid=%u geometryShader=%u vertexPipelineStoresAndAtomics=%u\n",
                    enabled.fillModeNonSolid, enabled.geometryShader, enabled.vertexPipelineStoresAndAtomics);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == count) fail("no graphics queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
        VkPhysicalDeviceDynamicRenderingFeatures rendering{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        rendering.dynamicRendering = VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &rendering};
        dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc; dc.pEnabledFeatures = &enabled;
        VK_CHECK(vkCreateDevice(gpu, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);

        createAttachment(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, target, targetView, targetMemory);
        VkMemoryRequirements req;

        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = byteCount;
        bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        VK_CHECK(vkCreateBuffer(device, &bc, nullptr, &buffer));
        vkGetBufferMemoryRequirements(device, buffer, &req);
        bufferMemory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VK_CHECK(vkBindBufferMemory(device, buffer, bufferMemory, 0));
        void *memory = nullptr;
        VK_CHECK(vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &memory));
        mapped = static_cast<uint8_t *>(memory);

        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (enabled.geometryShader && enabled.vertexPipelineStoresAndAtomics) {
            VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_GEOMETRY_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            sl.bindingCount = 1; sl.pBindings = &binding;
            VK_CHECK(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout));
            VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &psize;
            VK_CHECK(vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool));
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = descriptorPool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &setLayout;
            VK_CHECK(vkAllocateDescriptorSets(device, &alloc, &descriptorSet));
            VkDescriptorBufferInfo bi{buffer, 0, storeBytes};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = descriptorSet; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.descriptorCount = 1; write.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            lc.setLayoutCount = 1; lc.pSetLayouts = &setLayout;
        }
        VK_CHECK(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        VK_CHECK(vkCreateCommandPool(device, &pc, nullptr, &commandPool));
        VkCommandBufferAllocateInfo cc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cc.commandPool = commandPool; cc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cc, &cmd));
    }

    ~Fixture() {
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        vkUnmapMemory(device, bufferMemory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, bufferMemory, nullptr);
        vkDestroyImageView(device, targetView, nullptr);
        vkDestroyImage(device, target, nullptr);
        vkFreeMemory(device, targetMemory, nullptr);
        vkDestroyImageView(device, stencilView, nullptr);
        vkDestroyImage(device, stencil, nullptr);
        vkFreeMemory(device, stencilMemory, nullptr);
        vkDestroyImageView(device, depthView, nullptr);
        vkDestroyImage(device, depth, nullptr);
        vkFreeMemory(device, depthMemory, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    VkPipeline pipeline(const Test &test, VkPolygonMode mode) const {
        Case id = test.id;
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        auto stage = [&](VkShaderStageFlagBits type, const auto &code) {
            VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sc.pCode = code; sc.codeSize = sizeof(code);
            VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            st.stage = type; st.pName = "main";
            VK_CHECK(vkCreateShaderModule(device, &sc, nullptr, &st.module));
            stages.push_back(st);
        };
        VkBool32 clip = test.face == FaceTest::nearClip;
        VkSpecializationMapEntry entry{0, 0, sizeof(clip)};
        VkSpecializationInfo spec{1, &entry, sizeof(clip), &clip};
        bool ids = id == Case::primitiveID || id == Case::geometryID;
        if (id == Case::faces || ids) {
            stage(VK_SHADER_STAGE_VERTEX_BIT, faceVertexCode);
            stages.back().pSpecializationInfo = &spec;
        } else if (id == Case::depthSlope || id == Case::depthControl) {
            stage(VK_SHADER_STAGE_VERTEX_BIT, depthSlopeCode);
        } else {
            stage(VK_SHADER_STAGE_VERTEX_BIT, vertexCode);
        }
        if (id == Case::geometry || id == Case::geometryPoint) stage(VK_SHADER_STAGE_GEOMETRY_BIT, duplicateCode);
        if (id == Case::conditional) stage(VK_SHADER_STAGE_GEOMETRY_BIT, conditionalCode);
        if (id == Case::geometryID) stage(VK_SHADER_STAGE_GEOMETRY_BIT, primitiveIDGeometryCode);
        VkBool32 customIDs = id == Case::geometryID;
        VkSpecializationInfo fragmentSpec{1, &entry, sizeof(customIDs), &customIDs};
        if (id == Case::store) stage(VK_SHADER_STAGE_GEOMETRY_BIT, storeCode);
        else if (ids) {
            stage(VK_SHADER_STAGE_FRAGMENT_BIT, primitiveIDFragmentCode);
            stages.back().pSpecializationInfo = &fragmentSpec;
        }
        else if (id == Case::faces) stage(VK_SHADER_STAGE_FRAGMENT_BIT, faceFragmentCode);
        else stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentCode);
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = id == Case::store || id == Case::conditional ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, float(side), float(side), 0, 1};
        VkRect2D scissor{{0, 0}, {side, side}};
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vs.viewportCount = vs.scissorCount = 1; vs.pViewports = &vp; vs.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = mode; rs.lineWidth = 1; rs.rasterizerDiscardEnable = id == Case::store;
        rs.cullMode = (id == Case::lineCull || id == Case::pointCull) ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
        if (id == Case::faces) {
            rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
            if (test.face == FaceTest::cullBack) rs.cullMode = VK_CULL_MODE_BACK_BIT;
        }
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.stencilTestEnable = test.face == FaceTest::stencil;
        ds.front.compareMask = ds.back.compareMask = 0xff;
        ds.front.compareOp = ds.back.compareOp = VK_COMPARE_OP_EQUAL;
        ds.front.reference = 1; ds.back.reference = 2;
        ds.depthTestEnable = id == Case::depthSlope || id == Case::depthControl;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        rs.depthBiasEnable = id == Case::depthSlope;
        rs.depthBiasSlopeFactor = 12.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState state{}; state.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &state;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format;
        if (ds.stencilTestEnable) rendering.stencilAttachmentFormat = VK_FORMAT_S8_UINT;
        if (ds.depthTestEnable) rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = static_cast<uint32_t>(stages.size()); info.pStages = stages.data();
        info.pVertexInputState = &vi; info.pInputAssemblyState = &ia; info.pRasterizationState = &rs;
        info.pViewportState = &vs; info.pMultisampleState = &ms; info.pColorBlendState = &cb; info.layout = layout;
        if (ds.stencilTestEnable || ds.depthTestEnable) info.pDepthStencilState = &ds;
        VkPipeline result;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result));
        for (const auto &st : stages) vkDestroyShaderModule(device, st.module, nullptr);
        return result;
    }

    void transition(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags before, VkPipelineStageFlags after) const {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool run(const Test &test) {
        const Case id = test.id;
        VkPolygonMode mode = test.mode;
        if (id == Case::line || id == Case::lineCull) mode = VK_POLYGON_MODE_LINE;
        if (id == Case::point || id == Case::pointCull || id == Case::indexedPoint || id == Case::geometryPoint)
            mode = VK_POLYGON_MODE_POINT;
        // The fragment shader declaring PrimitiveId also requests SPIR-V Geometry.
        bool geometry = id == Case::geometry || id == Case::geometryPoint || id == Case::conditional ||
                        id == Case::store || id == Case::geometryID || id == Case::primitiveID;
        if ((mode != VK_POLYGON_MODE_FILL && !features.fillModeNonSolid) ||
            (geometry && !features.geometryShader) ||
            (id == Case::store && !features.vertexPipelineStoresAndAtomics)) {
            std::printf("UNSUPPORTED %s\n", test.name);
            return false;
        }
        if (test.face == FaceTest::stencil && !stencil)
            createAttachment(VK_FORMAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_STENCIL_BIT, stencil, stencilView, stencilMemory);
        bool depthTest = id == Case::depthSlope || id == Case::depthControl;
        if (depthTest && !depth)
            createAttachment(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, depth, depthView, depthMemory);
        VkPipeline p = pipeline(test, mode);
        std::memset(mapped, 0xcd, byteCount);
        const uint16_t indices[] = {0, 5, 3, 4};
        const VkDrawIndexedIndirectCommand args{3, id == Case::conditional ? 2u : 1u, 1, 1, 0};
        std::memcpy(mapped + indexOffset, indices, sizeof(indices));
        std::memcpy(mapped + argsOffset, &args, sizeof(args));
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = bufferMemory; range.size = VK_WHOLE_SIZE;
        VK_CHECK(vkFlushMappedMemoryRanges(device, 1, &range));
        VK_CHECK(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        transition(target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = targetView; attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color.float32[3] = 1;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {side, side}; rendering.layerCount = rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        VkRenderingAttachmentInfo st{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (test.face == FaceTest::stencil || depthTest) {
            transition(depthTest ? depth : stencil, depthTest ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
            st.imageView = depthTest ? depthView : stencilView; st.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            st.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; st.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            st.clearValue.depthStencil.stencil = 2; // Only the back-facing polygon matches.
            st.clearValue.depthStencil.depth = 0.4f;
            if (depthTest) rendering.pDepthAttachment = &st;
            else rendering.pStencilAttachment = &st;
        }
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        if (id == Case::store)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptorSet, 0, nullptr);
        if (id == Case::indexedPoint || id == Case::conditional) {
            vkCmdBindIndexBuffer(cmd, buffer, indexOffset, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexedIndirect(cmd, buffer, argsOffset, 1, sizeof(args));
        } else {
            bool pair = id == Case::faces || id == Case::primitiveID || id == Case::geometryID || depthTest;
            uint32_t vertices = id == Case::store ? storePoints : (pair ? 6u : 3u);
            vkCmdDraw(cmd, vertices, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
        transition(target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = imageOffset; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {side, side, 1};
        vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue));
        VK_CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));
        bool ok = checkReadback(test, mapped);
        vkDestroyPipeline(device, p, nullptr);
        return ok;
    }
};

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s DEVICE_SUBSTRING all", argv[0]);
        for (const Test &test : tests) std::fprintf(stderr, "|%s", test.name);
        std::fputc('\n', stderr);
        return 2;
    }
    Fixture fixture(argv[1]);
    bool matched = false, ok = true;
    for (const Test &test : tests) {
        if (!std::strcmp(argv[2], "all") || !std::strcmp(argv[2], test.name)) {
            matched = true;
            ok &= fixture.run(test);
        }
    }
    if (!matched) fail("unknown test case");
    return ok ? 0 : 1;
}
