#include "gpu.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "reports.hpp"
// generated from src/shaders at build time (cmake/spirv_to_c.cmake)
#include "shaders/decal_frag.h"
#include "shaders/decal_vert.h"
#include "shaders/flat_frag.h"
#include "shaders/flat_vert.h"
#include "shaders/room_frag.h"
#include "shaders/room_vert.h"
#include "shaders/warp_screen_frag.h"
#include "shaders/warp_tear_frag.h"
#include "shaders/warp_vert.h"
#include "shaders/water_frag.h"

// Vulkan structs are written `VkFoo x{VK_STRUCTURE_TYPE_FOO}`: the rest zero
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace gpu {
    namespace {
        constexpr uint32_t frames_in_flight = 2;
        constexpr VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkDeviceSize uniform_range = 1024; // the biggest uniform block a draw may pass
        constexpr uint32_t textures_per_draw = 3;    // bindings 1..3
        constexpr uint32_t sets_per_pool = 1024;

        // anything unexpected past initialisation is fatal
        void check(VkResult r, char const* what) {
            if (r == VK_SUCCESS)
                return;
            reports::add_log(std::string{"vulkan: "} + what + " failed (" + std::to_string(int(r)) + ")");
            std::fprintf(stderr, "vulkan: %s failed (%d)\n", what, int(r));
            std::abort();
        }

        struct buffer {
            VkBuffer buf = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            uint8_t* map = nullptr;
            VkDeviceSize size = 0;
        };

        struct image {
            VkImage img = VK_NULL_HANDLE;
            VkDeviceMemory mem = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            int width = 0, height = 0;
            int layers = 0; // 0: plain 2D
            sampler samp = sampler::nearest_clamp;
            bool used = false;
        };

        // per frame in flight: what the CPU writes while the GPU may still
        // read the other frame's
        struct frame {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            std::vector<VkDescriptorPool> pools;
            uint32_t pool_index = 0;
            buffer vertices, uniforms;
            VkDeviceSize vertices_used = 0, uniforms_used = 0;
            std::vector<buffer> garbage; // outgrown, freed once the frame is done
            // descriptor sets by their textures (the uniform buffer is the frame's)
            std::unordered_map<uint64_t, VkDescriptorSet> sets;
        };

        struct spirv {
            uint32_t const* code;
            size_t size;
        };
    }

    struct context {
        SDL_Window* window = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceMemoryProperties mem_props{};
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queue_family = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool cmd_pool = VK_NULL_HANDLE;

        // the window's swapchain
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkExtent2D swap_extent{};
        std::vector<VkImage> swap_images;
        std::vector<VkSemaphore> swap_done; // per image: the blit is done, present
        bool swap_dirty = false;

        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        std::array<VkPipeline, size_t(pipeline::count)> pipelines{};
        std::array<VkSampler, 3> samplers{};

        std::vector<image> textures; // texture id - 1
        texture white_2d = 0, white_array = 0; // stand-ins for unbound slots

        // the frame: drawn into `target`, `scene` its copy for the effects
        image target;
        VkImageLayout target_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        texture scene = 0;
        int width = 0, height = 0;
        std::array<frame, frames_in_flight> frames;
        uint32_t current = 0;
        bool recording = false;
        bool rendering = false;
        float clear[4] = {0.f, 0.f, 0.f, 1.f};
        VkDeviceSize vertex_base = 0; // of the last upload_vertices, in the frame's buffer

        buffer readback;
        bool readback_valid = false;
    };

    namespace {
        uint32_t find_memory(context const& c, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want = 0) {
            for (int pass = 0; pass < 2; ++pass) {
                VkMemoryPropertyFlags const flags = pass == 0 ? need | want : need;
                for (uint32_t i = 0; i < c.mem_props.memoryTypeCount; ++i)
                    if ((bits & (1u << i)) && (c.mem_props.memoryTypes[i].propertyFlags & flags) == flags)
                        return i;
            }
            check(VK_ERROR_OUT_OF_DEVICE_MEMORY, "finding a memory type");
            return 0;
        }

        // host visible, persistently mapped
        buffer make_buffer(context const& c, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want = 0) {
            buffer b;
            b.size = size;
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = size;
            bi.usage = usage;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(c.device, &bi, nullptr, &b.buf), "vkCreateBuffer");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(c.device, b.buf, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = find_memory(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, want);
            check(vkAllocateMemory(c.device, &ai, nullptr, &b.mem), "vkAllocateMemory (buffer)");
            check(vkBindBufferMemory(c.device, b.buf, b.mem, 0), "vkBindBufferMemory");
            void* p = nullptr;
            check(vkMapMemory(c.device, b.mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
            b.map = static_cast<uint8_t*>(p);
            return b;
        }

        void free_buffer(context const& c, buffer& b) {
            if (b.buf != VK_NULL_HANDLE)
                vkDestroyBuffer(c.device, b.buf, nullptr);
            if (b.mem != VK_NULL_HANDLE)
                vkFreeMemory(c.device, b.mem, nullptr);
            b = buffer{};
        }

        image make_image(context const& c, int w, int h, int layers, VkImageUsageFlags usage, VkFormat format, bool alpha_one = false) {
            image im;
            im.width = w;
            im.height = h;
            im.layers = layers;
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = format;
            ii.extent = VkExtent3D{uint32_t(w), uint32_t(h), 1};
            ii.mipLevels = 1;
            ii.arrayLayers = uint32_t(std::max(layers, 1));
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = usage;
            ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            check(vkCreateImage(c.device, &ii, nullptr, &im.img), "vkCreateImage");
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(c.device, im.img, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = find_memory(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(c.device, &ai, nullptr, &im.mem), "vkAllocateMemory (image)");
            check(vkBindImageMemory(c.device, im.img, im.mem, 0), "vkBindImageMemory");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im.img;
            vi.viewType = layers > 0 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.components = VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                alpha_one ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY};
            vi.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers};
            check(vkCreateImageView(c.device, &vi, nullptr, &im.view), "vkCreateImageView");
            im.used = true;
            return im;
        }

        void free_image(context const& c, image& im) {
            if (im.view != VK_NULL_HANDLE)
                vkDestroyImageView(c.device, im.view, nullptr);
            if (im.img != VK_NULL_HANDLE)
                vkDestroyImage(c.device, im.img, nullptr);
            if (im.mem != VK_NULL_HANDLE)
                vkFreeMemory(c.device, im.mem, nullptr);
            im = image{};
        }

        void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
            VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, uint32_t first_layer = 0, uint32_t layers = VK_REMAINING_ARRAY_LAYERS) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = src_stage;
            b.srcAccessMask = src_access;
            b.dstStageMask = dst_stage;
            b.dstAccessMask = dst_access;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, first_layer, layers};
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.imageMemoryBarrierCount = 1;
            d.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &d);
        }

        // one off commands (uploads), waited for
        template<class F>
        void immediate(context& c, F&& record) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = c.cmd_pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd;
            check(vkAllocateCommandBuffers(c.device, &ai, &cmd), "vkAllocateCommandBuffers");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
            record(cmd);
            check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            check(vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
            check(vkQueueWaitIdle(c.queue), "vkQueueWaitIdle");
            vkFreeCommandBuffers(c.device, c.cmd_pool, 1, &cmd);
        }

        image& tex(context& c, texture t) {
            return c.textures[t - 1];
        }

        texture add_texture(context& c, image im) {
            for (size_t i = 0; i < c.textures.size(); ++i)
                if (!c.textures[i].used) {
                    c.textures[i] = im;
                    return texture(i + 1);
                }
            c.textures.push_back(im);
            return texture(c.textures.size());
        }

        // (re)creates the scene copy and the frame target at the frame size
        void make_targets(context& c, int w, int h) {
            vkDeviceWaitIdle(c.device);
            free_image(c, c.target);
            c.target = make_image(c, w, h, 0, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, target_format);
            c.target_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (c.scene != 0)
                free_image(c, tex(c, c.scene));
            image sc = make_image(c, w, h, 0, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, target_format, true);
            sc.samp = sampler::linear_clamp;
            if (c.scene == 0)
                c.scene = add_texture(c, sc);
            else
                tex(c, c.scene) = sc;
            immediate(c, [&](VkCommandBuffer cmd) {
                barrier(cmd, sc.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkClearColorValue const black{};
                VkImageSubresourceRange const range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, sc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                barrier(cmd, sc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            });
            for (auto& f : c.frames)
                f.sets.clear();
            c.width = w;
            c.height = h;
        }

        // ---- swapchain

        void destroy_swapchain(context& c) {
            for (VkSemaphore s : c.swap_done)
                vkDestroySemaphore(c.device, s, nullptr);
            c.swap_done.clear();
            c.swap_images.clear();
            if (c.swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(c.device, c.swapchain, nullptr);
            c.swapchain = VK_NULL_HANDLE;
        }

        // false while the window has no area (minimised)
        bool make_swapchain(context& c) {
            vkDeviceWaitIdle(c.device);
            VkSurfaceCapabilitiesKHR caps;
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, c.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == 0xFFFFFFFFu) {
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(c.window, &w, &h);
                extent.width = std::clamp(uint32_t(std::max(w, 0)), caps.minImageExtent.width, caps.maxImageExtent.width);
                extent.height = std::clamp(uint32_t(std::max(h, 0)), caps.minImageExtent.height, caps.maxImageExtent.height);
            }
            if (extent.width == 0 || extent.height == 0)
                return false;

            uint32_t n = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, c.surface, &n, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(n);
            vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, c.surface, &n, formats.data());
            // plain UNORM like GL's default framebuffer: the blit then copies the values as they are
            VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
            for (auto const& f : formats)
                if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    format = f;
                    break;
                }

            VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            si.surface = c.surface;
            si.minImageCount = caps.maxImageCount == 0 ? caps.minImageCount + 1 : std::min(caps.minImageCount + 1, caps.maxImageCount);
            si.imageFormat = format.format;
            si.imageColorSpace = format.colorSpace;
            si.imageExtent = extent;
            si.imageArrayLayers = 1;
            si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            si.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
            si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
                for (uint32_t bit = 1; bit != 0 && bit <= caps.supportedCompositeAlpha; bit <<= 1)
                    if (caps.supportedCompositeAlpha & bit) {
                        si.compositeAlpha = VkCompositeAlphaFlagBitsKHR(bit);
                        break;
                    }
            si.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync, always there
            si.clipped = VK_TRUE;
            si.oldSwapchain = c.swapchain;
            VkSwapchainKHR sc;
            check(vkCreateSwapchainKHR(c.device, &si, nullptr, &sc), "vkCreateSwapchainKHR");
            destroy_swapchain(c);
            c.swapchain = sc;
            c.swap_extent = extent;
            vkGetSwapchainImagesKHR(c.device, sc, &n, nullptr);
            c.swap_images.resize(n);
            vkGetSwapchainImagesKHR(c.device, sc, &n, c.swap_images.data());
            c.swap_done.resize(n);
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            for (auto& s : c.swap_done)
                check(vkCreateSemaphore(c.device, &semi, nullptr, &s), "vkCreateSemaphore");
            c.swap_dirty = false;
            return true;
        }

        // ---- pipelines

        VkShaderModule make_module(context const& c, spirv s) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = s.size;
            mi.pCode = s.code;
            VkShaderModule m;
            check(vkCreateShaderModule(c.device, &mi, nullptr, &m), "vkCreateShaderModule");
            return m;
        }

        void make_pipelines(context& c) {
            std::array<VkDescriptorSetLayoutBinding, 1 + textures_per_draw> bindings{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            for (uint32_t i = 1; i <= textures_per_draw; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = uint32_t(bindings.size());
            li.pBindings = bindings.data();
            check(vkCreateDescriptorSetLayout(c.device, &li, nullptr, &c.set_layout), "vkCreateDescriptorSetLayout");
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &c.set_layout;
            check(vkCreatePipelineLayout(c.device, &pli, nullptr, &c.layout), "vkCreatePipelineLayout");

            VkVertexInputBindingDescription const vb{0, sizeof(vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription const va[3] = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(vertex, x))},
                {1, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(vertex, rgba))},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(vertex, u))},
            };
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = 3;
            vi.pVertexAttributeDescriptions = va;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), alpha included
            VkPipelineColorBlendAttachmentState blend{};
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &blend;
            VkDynamicState const dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            ds.dynamicStateCount = 2;
            ds.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            ri.colorAttachmentCount = 1;
            ri.pColorAttachmentFormats = &target_format;

            spirv const warp_vert{warp_vert_spv, warp_vert_spv_size};
            std::array<std::array<spirv, 2>, size_t(pipeline::count)> const sources = {{
                {spirv{flat_vert_spv, flat_vert_spv_size}, spirv{flat_frag_spv, flat_frag_spv_size}},
                {spirv{room_vert_spv, room_vert_spv_size}, spirv{room_frag_spv, room_frag_spv_size}},
                {spirv{decal_vert_spv, decal_vert_spv_size}, spirv{decal_frag_spv, decal_frag_spv_size}},
                {warp_vert, spirv{warp_tear_frag_spv, warp_tear_frag_spv_size}},
                {warp_vert, spirv{warp_screen_frag_spv, warp_screen_frag_spv_size}},
                {warp_vert, spirv{water_frag_spv, water_frag_spv_size}},
            }};
            for (size_t i = 0; i < sources.size(); ++i) {
                VkShaderModule const vs = make_module(c, sources[i][0]);
                VkShaderModule const fs = make_module(c, sources[i][1]);
                VkPipelineShaderStageCreateInfo stages[2]{};
                stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = vs;
                stages[0].pName = "main";
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = fs;
                stages[1].pName = "main";
                VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                pi.pNext = &ri;
                pi.stageCount = 2;
                pi.pStages = stages;
                pi.pVertexInputState = &vi;
                pi.pInputAssemblyState = &ia;
                pi.pViewportState = &vp;
                pi.pRasterizationState = &rs;
                pi.pMultisampleState = &ms;
                pi.pColorBlendState = &cb;
                pi.pDynamicState = &ds;
                pi.layout = c.layout;
                check(vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &pi, nullptr, &c.pipelines[i]), "vkCreateGraphicsPipelines");
                vkDestroyShaderModule(c.device, vs, nullptr);
                vkDestroyShaderModule(c.device, fs, nullptr);
            }

            auto const make_sampler = [&](VkFilter filter, VkSamplerAddressMode mode) {
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                si.magFilter = si.minFilter = filter;
                si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU = si.addressModeV = si.addressModeW = mode;
                si.maxLod = 0.f;
                VkSampler s;
                check(vkCreateSampler(c.device, &si, nullptr, &s), "vkCreateSampler");
                return s;
            };
            c.samplers[size_t(sampler::nearest_clamp)] = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            c.samplers[size_t(sampler::nearest_repeat)] = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
            c.samplers[size_t(sampler::linear_clamp)] = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        }

        // ---- device

        bool pick_device(context& c, bool present) {
            uint32_t n = 0;
            vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
            std::vector<VkPhysicalDevice> devs(n);
            vkEnumeratePhysicalDevices(c.instance, &n, devs.data());
            int best_score = -1;
            for (VkPhysicalDevice d : devs) {
                VkPhysicalDeviceProperties p;
                vkGetPhysicalDeviceProperties(d, &p);
                if (p.apiVersion < VK_API_VERSION_1_3)
                    continue;
                VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f.pNext = &f13;
                vkGetPhysicalDeviceFeatures2(d, &f);
                if (!f13.dynamicRendering || !f13.synchronization2)
                    continue;
                if (present) {
                    uint32_t ne = 0;
                    vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
                    std::vector<VkExtensionProperties> exts(ne);
                    vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, exts.data());
                    if (std::none_of(exts.begin(), exts.end(), [](auto const& e) { return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }))
                        continue;
                }
                uint32_t nq = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
                std::vector<VkQueueFamilyProperties> qs(nq);
                vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
                for (uint32_t q = 0; q < nq; ++q) {
                    if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                        continue;
                    VkBool32 can_present = VK_TRUE;
                    if (present)
                        vkGetPhysicalDeviceSurfaceSupportKHR(d, q, c.surface, &can_present);
                    if (!can_present)
                        continue;
                    int const score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU           ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU              ? 2
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU                      ? 1
                                                                                           : 0;
                    if (score > best_score) {
                        best_score = score;
                        c.phys = d;
                        c.queue_family = q;
                    }
                    break;
                }
            }
            if (c.phys == VK_NULL_HANDLE) {
                reports::add_log("vulkan: no Vulkan 1.3 device with dynamic rendering and synchronization2" + std::string{present ? " that can present to the window" : ""});
                return false;
            }
            vkGetPhysicalDeviceProperties(c.phys, &c.props);
            vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem_props);
            reports::add_log(std::string{"vulkan: "} + c.props.deviceName);
            return true;
        }

        // ---- frame recording

        frame& cur(context& c) {
            return c.frames[c.current];
        }

        void begin_commands(context& c) {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkResetCommandBuffer(cur(c).cmd, 0), "vkResetCommandBuffer");
            check(vkBeginCommandBuffer(cur(c).cmd, &bi), "vkBeginCommandBuffer");
            c.recording = true;
        }

        // into the target, keeping what it has (or clearing it, at the frame's start)
        void resume(context& c, bool clear) {
            if (c.rendering)
                return;
            VkCommandBuffer const cmd = cur(c).cmd;
            if (c.target_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                // after the previous frame's (or this one's) copies out of it, in queue order
                barrier(cmd, c.target.img, clear ? VK_IMAGE_LAYOUT_UNDEFINED : c.target_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                c.target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = c.target.view;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            std::memcpy(att.clearValue.color.float32, c.clear, sizeof(c.clear));
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = VkRect2D{{0, 0}, {uint32_t(c.width), uint32_t(c.height)}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &ri);
            // GL's orientation: y up in clip space
            VkViewport const vp{0.f, float(c.height), float(c.width), -float(c.height), 0.f, 1.f};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D const sc{{0, 0}, {uint32_t(c.width), uint32_t(c.height)}};
            vkCmdSetScissor(cmd, 0, 1, &sc);
            c.rendering = true;
        }

        void suspend(context& c) {
            if (!c.rendering)
                return;
            vkCmdEndRendering(cur(c).cmd);
            c.rendering = false;
        }

        // the target as a transfer source (from rendering)
        void target_to_transfer(context& c) {
            suspend(c);
            barrier(cur(c).cmd, c.target.img, c.target_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            c.target_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }

        void submit(context& c, VkSemaphore wait, VkSemaphore signal) {
            frame& f = cur(c);
            check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");
            c.recording = false;
            VkPipelineStageFlags const wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0;
            si.pWaitSemaphores = &wait;
            si.pWaitDstStageMask = &wait_stage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &f.cmd;
            si.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1 : 0;
            si.pSignalSemaphores = &signal;
            check(vkQueueSubmit(c.queue, 1, &si, f.fence), "vkQueueSubmit");
        }

        // `size` bytes out of a frame's stream buffer, growing it (the old one
        // lives on until the frame is done)
        VkDeviceSize stream(context& c, buffer& b, VkDeviceSize& used, VkDeviceSize size, VkDeviceSize align, VkDeviceSize reserve, VkBufferUsageFlags usage) {
            VkDeviceSize const at = (used + align - 1) / align * align;
            if (at + std::max(size, reserve) > b.size) {
                VkDeviceSize grown = std::max<VkDeviceSize>(b.size * 2, 1u << 20);
                while (grown < std::max(size, reserve))
                    grown *= 2;
                cur(c).garbage.push_back(b);
                b = make_buffer(c, grown, usage);
                used = 0;
                cur(c).sets.clear(); // they point at the old uniform buffer
                return stream(c, b, used, size, align, reserve, usage);
            }
            used = at + size;
            return at;
        }

        VkDescriptorSet descriptor_set(context& c, std::array<texture, textures_per_draw> const& t) {
            frame& f = cur(c);
            uint64_t const key = uint64_t(t[0]) | (uint64_t(t[1]) << 21) | (uint64_t(t[2]) << 42);
            if (auto const it = f.sets.find(key); it != f.sets.end())
                return it->second;
            VkDescriptorSet set = VK_NULL_HANDLE;
            while (true) {
                if (f.pool_index == f.pools.size()) {
                    VkDescriptorPoolSize const sizes[2] = {
                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, sets_per_pool},
                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets_per_pool * textures_per_draw},
                    };
                    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    pi.maxSets = sets_per_pool;
                    pi.poolSizeCount = 2;
                    pi.pPoolSizes = sizes;
                    VkDescriptorPool pool;
                    check(vkCreateDescriptorPool(c.device, &pi, nullptr, &pool), "vkCreateDescriptorPool");
                    f.pools.push_back(pool);
                }
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = f.pools[f.pool_index];
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &c.set_layout;
                VkResult const r = vkAllocateDescriptorSets(c.device, &ai, &set);
                if (r == VK_SUCCESS)
                    break;
                if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                    check(r, "vkAllocateDescriptorSets");
                ++f.pool_index;
            }
            VkDescriptorBufferInfo const ub{f.uniforms.buf, 0, uniform_range};
            std::array<VkDescriptorImageInfo, textures_per_draw> images;
            std::array<VkWriteDescriptorSet, 1 + textures_per_draw> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[0].pBufferInfo = &ub;
            for (uint32_t i = 0; i < textures_per_draw; ++i) {
                image const& im = tex(c, t[i]);
                images[i] = VkDescriptorImageInfo{c.samplers[size_t(im.samp)], im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                auto& w = writes[i + 1];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = i + 1;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(c.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
            f.sets.emplace(key, set);
            return set;
        }

        // waits for the frame slot's last use and makes it ready to record
        void reuse_frame(context& c) {
            frame& f = cur(c);
            check(vkWaitForFences(c.device, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
            check(vkResetFences(c.device, 1, &f.fence), "vkResetFences");
            for (auto& b : f.garbage)
                free_buffer(c, b);
            f.garbage.clear();
            for (VkDescriptorPool p : f.pools)
                vkResetDescriptorPool(c.device, p, 0);
            f.pool_index = 0;
            f.sets.clear();
            f.vertices_used = 0;
            f.uniforms_used = 0;
        }
    }

    context* create(SDL_Window* window, bool present) {
        auto* c = new context;
        c->window = window;
        uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
            reports::add_log("vulkan: the loader doesn't support Vulkan 1.3");
            delete c;
            return nullptr;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "rwcpp";
        app.pEngineName = "rwcpp";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        if (present) {
            Uint32 n = 0;
            char const* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
            if (exts == nullptr) {
                reports::add_log(std::string{"SDL_Vulkan_GetInstanceExtensions: "} + SDL_GetError());
                delete c;
                return nullptr;
            }
            ii.enabledExtensionCount = n;
            ii.ppEnabledExtensionNames = exts;
        }
        if (VkResult const r = vkCreateInstance(&ii, nullptr, &c->instance); r != VK_SUCCESS) {
            reports::add_log("vulkan: vkCreateInstance failed (" + std::to_string(int(r)) + ")");
            delete c;
            return nullptr;
        }
        if (present && !SDL_Vulkan_CreateSurface(window, c->instance, nullptr, &c->surface)) {
            reports::add_log(std::string{"SDL_Vulkan_CreateSurface: "} + SDL_GetError());
            destroy(c);
            return nullptr;
        }
        if (!pick_device(*c, present)) {
            destroy(c);
            return nullptr;
        }

        float const priority = 1.f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = c->queue_family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        char const* const swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &f13;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = present ? 1 : 0;
        di.ppEnabledExtensionNames = &swapchain_ext;
        if (VkResult const r = vkCreateDevice(c->phys, &di, nullptr, &c->device); r != VK_SUCCESS) {
            reports::add_log("vulkan: vkCreateDevice failed (" + std::to_string(int(r)) + ")");
            destroy(c);
            return nullptr;
        }
        vkGetDeviceQueue(c->device, c->queue_family, 0, &c->queue);

        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = c->queue_family;
        check(vkCreateCommandPool(c->device, &pi, nullptr, &c->cmd_pool), "vkCreateCommandPool");
        for (auto& f : c->frames) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = c->cmd_pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(c->device, &ai, &f.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(c->device, &fi, nullptr, &f.fence), "vkCreateFence");
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(c->device, &si, nullptr, &f.acquired), "vkCreateSemaphore");
            f.vertices = make_buffer(*c, 4u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            f.uniforms = make_buffer(*c, 1u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        }
        make_pipelines(*c);

        uint8_t const white[4] = {255, 255, 255, 255};
        c->white_2d = create_texture(*c, 1, 1, 0, sampler::nearest_clamp);
        upload(*c, c->white_2d, 0, white, 4);
        c->white_array = create_texture(*c, 1, 1, 1, sampler::nearest_clamp);
        upload(*c, c->white_array, 0, white, 4);
        return c;
    }

    void destroy(context* c) {
        if (c == nullptr)
            return;
        if (c->device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(c->device);
            for (auto& f : c->frames) {
                for (auto& b : f.garbage)
                    free_buffer(*c, b);
                free_buffer(*c, f.vertices);
                free_buffer(*c, f.uniforms);
                for (VkDescriptorPool p : f.pools)
                    vkDestroyDescriptorPool(c->device, p, nullptr);
                if (f.fence != VK_NULL_HANDLE)
                    vkDestroyFence(c->device, f.fence, nullptr);
                if (f.acquired != VK_NULL_HANDLE)
                    vkDestroySemaphore(c->device, f.acquired, nullptr);
            }
            free_buffer(*c, c->readback);
            for (auto& im : c->textures)
                free_image(*c, im);
            free_image(*c, c->target);
            for (VkSampler s : c->samplers)
                if (s != VK_NULL_HANDLE)
                    vkDestroySampler(c->device, s, nullptr);
            for (VkPipeline p : c->pipelines)
                if (p != VK_NULL_HANDLE)
                    vkDestroyPipeline(c->device, p, nullptr);
            if (c->layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(c->device, c->layout, nullptr);
            if (c->set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(c->device, c->set_layout, nullptr);
            destroy_swapchain(*c);
            if (c->cmd_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(c->device, c->cmd_pool, nullptr);
            vkDestroyDevice(c->device, nullptr);
        }
        if (c->surface != VK_NULL_HANDLE)
            SDL_Vulkan_DestroySurface(c->instance, c->surface, nullptr);
        if (c->instance != VK_NULL_HANDLE)
            vkDestroyInstance(c->instance, nullptr);
        delete c;
    }

    texture create_texture(context& c, int width, int height, int layers, sampler s) {
        auto const& lim = c.props.limits;
        if (width <= 0 || height <= 0 || layers < 0 || uint32_t(width) > lim.maxImageDimension2D || uint32_t(height) > lim.maxImageDimension2D
            || uint32_t(layers) > lim.maxImageArrayLayers) {
            reports::add_log("vulkan: a " + std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(layers) + " texture is beyond what the device can do");
            return 0;
        }
        image im = make_image(c, width, height, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_R8G8B8A8_UNORM);
        im.samp = s;
        immediate(c, [&](VkCommandBuffer cmd) {
            barrier(cmd, im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue const black{};
            VkImageSubresourceRange const range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            barrier(cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        return add_texture(c, im);
    }

    void upload(context& c, texture t, int layer, uint8_t const* pixels, int channels) {
        if (t == 0)
            return;
        image const& im = tex(c, t);
        size_t const texels = size_t(im.width) * size_t(im.height);
        buffer staging = make_buffer(c, texels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (channels == 4) {
            std::memcpy(staging.map, pixels, texels * 4);
        } else {
            for (size_t i = 0; i < texels; ++i) {
                staging.map[i * 4 + 0] = pixels[i * 3 + 0];
                staging.map[i * 4 + 1] = pixels[i * 3 + 1];
                staging.map[i * 4 + 2] = pixels[i * 3 + 2];
                staging.map[i * 4 + 3] = 255;
            }
        }
        immediate(c, [&](VkCommandBuffer cmd) {
            barrier(cmd, im.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, uint32_t(layer), 1);
            VkBufferImageCopy region{};
            region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(layer), 1};
            region.imageExtent = VkExtent3D{uint32_t(im.width), uint32_t(im.height), 1};
            vkCmdCopyBufferToImage(cmd, staging.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, uint32_t(layer), 1);
        });
        free_buffer(c, staging);
    }

    void destroy_texture(context& c, texture t) {
        if (t == 0 || t > c.textures.size() || !tex(c, t).used)
            return;
        // rare (a room change): simply wait for the frames that may use it
        vkDeviceWaitIdle(c.device);
        free_image(c, tex(c, t));
        for (auto& f : c.frames)
            f.sets.clear();
    }

    uint32_t max_texture_size(context const& c) {
        return c.props.limits.maxImageDimension2D;
    }

    void begin_frame(context& c, int width, int height, float r, float g, float b) {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (c.recording) {
            // a frame that was never presented (a screenshot): drop it
            suspend(c);
            submit(c, VK_NULL_HANDLE, VK_NULL_HANDLE);
            c.current = (c.current + 1) % frames_in_flight;
        }
        reuse_frame(c);
        if (width != c.width || height != c.height)
            make_targets(c, width, height);
        c.clear[0] = r;
        c.clear[1] = g;
        c.clear[2] = b;
        c.clear[3] = 1.f;
        c.readback_valid = false;
        begin_commands(c);
        resume(c, true);
    }

    void upload_vertices(context& c, vertex const* v, size_t count) {
        frame& f = cur(c);
        size_t const bytes = std::max<size_t>(count, 1) * sizeof(vertex);
        c.vertex_base = stream(c, f.vertices, f.vertices_used, bytes, sizeof(vertex), 0, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (count > 0)
            std::memcpy(f.vertices.map + c.vertex_base, v, count * sizeof(vertex));
    }

    void draw(context& c, pipeline p, void const* uniforms, size_t size, std::initializer_list<texture> textures, size_t first, size_t count) {
        if (count == 0)
            return;
        frame& f = cur(c);
        resume(c, false);
        c.readback_valid = false;
        VkDeviceSize const ubo = stream(c, f.uniforms, f.uniforms_used, std::min<size_t>(size, uniform_range), c.props.limits.minUniformBufferOffsetAlignment,
            uniform_range, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        std::memcpy(f.uniforms.map + ubo, uniforms, std::min<size_t>(size, uniform_range));
        // unused slots: whichever kind the shader declares there, nothing reads them
        std::array<texture, textures_per_draw> t{c.white_2d, c.white_array, c.white_2d};
        size_t i = 0;
        for (texture const x : textures)
            if (i < textures_per_draw) {
                if (x != 0 && x <= c.textures.size() && tex(c, x).used)
                    t[i] = x;
                ++i;
            }
        VkDescriptorSet const set = descriptor_set(c, t);
        VkCommandBuffer const cmd = f.cmd;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipelines[size_t(p)]);
        uint32_t const dyn = uint32_t(ubo);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1, &set, 1, &dyn);
        VkDeviceSize const offset = c.vertex_base;
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.vertices.buf, &offset);
        vkCmdDraw(cmd, uint32_t(count), 1, uint32_t(first), 0);
    }

    texture grab_scene(context& c) {
        target_to_transfer(c);
        VkCommandBuffer const cmd = cur(c).cmd;
        image const& sc = tex(c, c.scene);
        // (after the draws that sampled the previous copy)
        barrier(cmd, sc.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        // turned upside down: bottom row first, like glCopyTexSubImage2D
        VkImageBlit region{};
        region.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = VkOffset3D{0, 0, 0};
        region.srcOffsets[1] = VkOffset3D{c.width, c.height, 1};
        region.dstSubresource = region.srcSubresource;
        region.dstOffsets[0] = VkOffset3D{0, c.height, 0};
        region.dstOffsets[1] = VkOffset3D{c.width, 0, 1};
        vkCmdBlitImage(cmd, c.target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        barrier(cmd, sc.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // (the target goes back to rendering with the next draw)
        return c.scene;
    }

    uint8_t const* read_back(context& c) {
        if (c.readback_valid)
            return c.readback.map;
        if (!c.recording)
            return nullptr; // no frame
        VkDeviceSize const bytes = VkDeviceSize(c.width) * VkDeviceSize(c.height) * 4;
        if (c.readback.size < bytes) {
            free_buffer(c, c.readback);
            c.readback = make_buffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        target_to_transfer(c);
        VkCommandBuffer const cmd = cur(c).cmd;
        VkBufferImageCopy region{};
        region.imageSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = VkExtent3D{uint32_t(c.width), uint32_t(c.height), 1};
        vkCmdCopyImageToBuffer(cmd, c.target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.readback.buf, 1, &region);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
        // run what was recorded so far and carry on in the same frame
        submit(c, VK_NULL_HANDLE, VK_NULL_HANDLE);
        frame& f = cur(c);
        check(vkWaitForFences(c.device, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(c.device, 1, &f.fence), "vkResetFences");
        begin_commands(c);
        c.readback_valid = true;
        return c.readback.map;
    }

    void present(context& c) {
        if (!c.recording)
            return;
        frame& f = cur(c);
        uint32_t index = 0;
        bool shown = false;
        if (c.surface != VK_NULL_HANDLE) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(c.window, &w, &h);
            if (c.swapchain == VK_NULL_HANDLE || c.swap_dirty || uint32_t(w) != c.swap_extent.width || uint32_t(h) != c.swap_extent.height) {
                // (waits for the GPU: nothing of this frame is submitted yet but its uploads)
                if (w > 0 && h > 0)
                    make_swapchain(c);
            }
            if (c.swapchain != VK_NULL_HANDLE) {
                VkResult const r = vkAcquireNextImageKHR(c.device, c.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
                    shown = true;
                    c.swap_dirty = r == VK_SUBOPTIMAL_KHR;
                } else if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                    c.swap_dirty = true;
                } else {
                    check(r, "vkAcquireNextImageKHR");
                }
            }
        }
        if (shown) {
            target_to_transfer(c);
            VkImage const dst = c.swap_images[index];
            barrier(f.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageBlit region{};
            region.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = VkOffset3D{c.width, c.height, 1};
            region.dstSubresource = region.srcSubresource;
            region.dstOffsets[1] = VkOffset3D{int32_t(c.swap_extent.width), int32_t(c.swap_extent.height), 1};
            vkCmdBlitImage(f.cmd, c.target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0);
            submit(c, f.acquired, c.swap_done[index]);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &c.swap_done[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &c.swapchain;
            pi.pImageIndices = &index;
            VkResult const r = vkQueuePresentKHR(c.queue, &pi);
            if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
                c.swap_dirty = true;
            else
                check(r, "vkQueuePresentKHR");
        } else {
            suspend(c);
            submit(c, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
        c.current = (c.current + 1) % frames_in_flight;
    }
}
