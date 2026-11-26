#define VK_USE_PLATFORM_ANDROID_KHR

#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VulkanRenderer {
public:
    std::string start(
            JNIEnv* env,
            jobject surfaceObject,
            jobject assetManagerObject,
            bool halfResolution,
            uint32_t lightingMode) {
        stop();
        halfResolution_ = halfResolution;
        lightingMode_ = std::min(lightingMode, 2U);

        window_ = ANativeWindow_fromSurface(env, surfaceObject);
        if (window_ == nullptr) {
            return abortStart("ERROR: Could not obtain ANativeWindow from Surface.");
        }

        AAssetManager* assetManager = AAssetManager_fromJava(env, assetManagerObject);
        if (assetManager == nullptr) {
            return abortStart("ERROR: Could not access Android shader assets.");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "67xFrames";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 4, 0);
        appInfo.pEngineName = "67xFrames";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 4, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
        };

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;

        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            return abortStart("ERROR: vkCreateInstance failed: " + vkResultName(result));
        }

        VkAndroidSurfaceCreateInfoKHR androidSurfaceInfo{};
        androidSurfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        androidSurfaceInfo.window = window_;

        result = vkCreateAndroidSurfaceKHR(
            instance_, &androidSurfaceInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            return abortStart(
                "ERROR: vkCreateAndroidSurfaceKHR failed: " + vkResultName(result));
        }

        if (!choosePhysicalDevice()) return abortStart(status_);
        if (!createDevice()) return abortStart(status_);
        if (!createSwapchain()) return abortStart(status_);
        if (!createOffscreenImage()) return abortStart(status_);
        if (!createRenderPassAndFramebuffer()) return abortStart(status_);
        if (!createGraphicsPipeline(assetManager)) return abortStart(status_);
        if (!createFrameResources()) return abortStart(status_);

        status_ = "READY\n67xFrames — procedural Vulkan scene\n";
        status_ += "GPU: ";
        status_ += properties_.deviceName;
        status_ += "\nDevice API: ";
        status_ += std::to_string(VK_VERSION_MAJOR(properties_.apiVersion));
        status_ += ".";
        status_ += std::to_string(VK_VERSION_MINOR(properties_.apiVersion));
        status_ += ".";
        status_ += std::to_string(VK_VERSION_PATCH(properties_.apiVersion));
        status_ += "\nOffscreen image: ";
        status_ += std::to_string(offscreenExtent_.width);
        status_ += " x ";
        status_ += std::to_string(offscreenExtent_.height);
        status_ += "\nUpscaled to: ";
        status_ += std::to_string(extent_.width);
        status_ += " x ";
        status_ += std::to_string(extent_.height);
        status_ += "\nGraphics pipeline: ready";
        status_ += "\nDraw: 3 vertices (no vertex buffer)";
        status_ += "\nUpscale filter: ";
        status_ += (blitFilter_ == VK_FILTER_LINEAR ? "linear blit" : "nearest blit");
        status_ += "\nSwapchain images: ";
        status_ += std::to_string(images_.size());
        status_ += "\nScene: animated ray-marched tunnel";
        status_ += halfResolution_ ? "\nMode: half resolution → upscale" : "\nMode: native resolution";
        status_ += "\nLighting: ";
        status_ += lightingMode_ == 0U ? "OFF" : (lightingMode_ == 1U ? "PARTIAL" : "MAX");
        status_ += "\nGPU timing: Vulkan timestamps when supported";
        return status_;
    }

    bool drawFrame() {
        if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE ||
            offscreenImage_ == VK_NULL_HANDLE || graphicsPipeline_ == VK_NULL_HANDLE) {
            status_ = "ERROR: Renderer is not initialized.";
            return false;
        }

        VkResult result = vkWaitForFences(
            device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: Waiting for frame fence failed: " + vkResultName(result));
        }

        // The previous frame is complete, so its timestamp pair is safe to read.
        if (timestampQueryPool_ != VK_NULL_HANDLE && frameNumber_ > 0) {
            uint64_t timestamps[2]{};
            const VkResult queryResult = vkGetQueryPoolResults(
                device_, timestampQueryPool_, 0, 2,
                sizeof(timestamps), timestamps, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (queryResult == VK_SUCCESS) {
                uint64_t ticks = timestamps[1] - timestamps[0];
                if (timestampValidBits_ > 0 && timestampValidBits_ < 64) {
                    const uint64_t mask = (1ULL << timestampValidBits_) - 1ULL;
                    ticks &= mask;
                }
                latestGpuMs_ = static_cast<float>(
                    static_cast<double>(ticks) *
                    static_cast<double>(properties_.limits.timestampPeriod) / 1000000.0);
            }
        }

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX,
            imageAvailable_, VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return failFrame("ERROR: Swapchain is out of date. Reopen the app to recreate it.");
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return failFrame("ERROR: vkAcquireNextImageKHR failed: " + vkResultName(result));
        }

        result = vkResetCommandBuffer(commandBuffer_, 0);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkResetCommandBuffer failed: " + vkResultName(result));
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkBeginCommandBuffer failed: " + vkResultName(result));
        }

        // Render a real three-vertex triangle into the half-resolution image.
        // The render pass transitions its final layout to TRANSFER_SRC_OPTIMAL.
        VkClearValue clearValue{};
        const float phase = static_cast<float>(frameNumber_ % 360U) *
                            0.01745329251994329577f;
        clearValue.color.float32[0] = 0.025f + 0.025f * (0.5f + 0.5f * std::sin(phase));
        clearValue.color.float32[1] = 0.035f + 0.030f * (0.5f + 0.5f * std::sin(phase + 2.1f));
        clearValue.color.float32[2] = 0.070f + 0.040f * (0.5f + 0.5f * std::sin(phase + 4.2f));
        clearValue.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo renderPassBegin{};
        renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBegin.renderPass = renderPass_;
        renderPassBegin.framebuffer = framebuffer_;
        renderPassBegin.renderArea.offset = {0, 0};
        renderPassBegin.renderArea.extent = offscreenExtent_;
        renderPassBegin.clearValueCount = 1;
        renderPassBegin.pClearValues = &clearValue;

        if (timestampQueryPool_ != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(commandBuffer_, timestampQueryPool_, 0, 2);
            vkCmdWriteTimestamp(
                commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                timestampQueryPool_, 0);
        }

        vkCmdBeginRenderPass(commandBuffer_, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(
            commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

        const float scenePushData[3] = {
            static_cast<float>(frameNumber_) * (1.0f / 60.0f),
            static_cast<float>(offscreenExtent_.width) /
                static_cast<float>(std::max(1U, offscreenExtent_.height)),
            static_cast<float>(lightingMode_)
        };
        vkCmdPushConstants(
            commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(scenePushData), scenePushData);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer_);

        if (timestampQueryPool_ != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(
                commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                timestampQueryPool_, 1);
        }

        // Make the acquired swapchain image a transfer destination.
        VkImageMemoryBarrier swapchainToDst{};
        swapchainToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapchainToDst.srcAccessMask = 0;
        swapchainToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        swapchainToDst.oldLayout = imageLayouts_[imageIndex];
        swapchainToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapchainToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapchainToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapchainToDst.image = images_[imageIndex];
        swapchainToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        swapchainToDst.subresourceRange.baseMipLevel = 0;
        swapchainToDst.subresourceRange.levelCount = 1;
        swapchainToDst.subresourceRange.baseArrayLayer = 0;
        swapchainToDst.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            commandBuffer_,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &swapchainToDst);

        // Scale the real triangle image to the full swapchain dimensions.
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = 0;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {
            static_cast<int32_t>(offscreenExtent_.width),
            static_cast<int32_t>(offscreenExtent_.height),
            1
        };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = 0;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {
            static_cast<int32_t>(extent_.width),
            static_cast<int32_t>(extent_.height),
            1
        };

        vkCmdBlitImage(
            commandBuffer_,
            offscreenImage_,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            images_[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            blitFilter_);

        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = images_[imageIndex];
        toPresent.subresourceRange = swapchainToDst.subresourceRange;

        vkCmdPipelineBarrier(
            commandBuffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toPresent);

        result = vkEndCommandBuffer(commandBuffer_);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkEndCommandBuffer failed: " + vkResultName(result));
        }

        result = vkResetFences(device_, 1, &inFlightFence_);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkResetFences failed: " + vkResultName(result));
        }

        // Wait for the acquired swapchain image only at the transfer stage.
        // The offscreen triangle pass can execute before that wait is needed.
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailable_;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished_[imageIndex];

        result = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFence_);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkQueueSubmit failed: " + vkResultName(result));
        }

        imageLayouts_[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        ++frameNumber_;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished_[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            return failFrame("ERROR: Swapchain changed size/state. Reopen the app to recreate it.");
        }
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: vkQueuePresentKHR failed: " + vkResultName(result));
        }
        return true;
    }

    std::string lastStatus() const {
        std::string report = status_;
        if (timestampQueryPool_ != VK_NULL_HANDLE) {
            if (latestGpuMs_ >= 0.0f) {
                report += "\nGPU scene pass: " + std::to_string(latestGpuMs_) + " ms";
            } else {
                report += "\nGPU scene pass: collecting samples...";
            }
        } else {
            report += "\nGPU timestamps: unavailable on this queue";
        }
        return report;
    }

    void stop() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

            if (timestampQueryPool_ != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device_, timestampQueryPool_, nullptr);
                timestampQueryPool_ = VK_NULL_HANDLE;
            }

            for (VkSemaphore semaphore : renderFinished_) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_, semaphore, nullptr);
                }
            }
            renderFinished_.clear();

            if (imageAvailable_ != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, imageAvailable_, nullptr);
                imageAvailable_ = VK_NULL_HANDLE;
            }
            if (inFlightFence_ != VK_NULL_HANDLE) {
                vkDestroyFence(device_, inFlightFence_, nullptr);
                inFlightFence_ = VK_NULL_HANDLE;
            }
            if (commandPool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, commandPool_, nullptr);
                commandPool_ = VK_NULL_HANDLE;
                commandBuffer_ = VK_NULL_HANDLE;
            }

            if (graphicsPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
                graphicsPipeline_ = VK_NULL_HANDLE;
            }
            if (pipelineLayout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
                pipelineLayout_ = VK_NULL_HANDLE;
            }
            if (framebuffer_ != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer_, nullptr);
                framebuffer_ = VK_NULL_HANDLE;
            }
            if (imageView_ != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, imageView_, nullptr);
                imageView_ = VK_NULL_HANDLE;
            }
            if (renderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, renderPass_, nullptr);
                renderPass_ = VK_NULL_HANDLE;
            }
            if (offscreenImage_ != VK_NULL_HANDLE) {
                vkDestroyImage(device_, offscreenImage_, nullptr);
                offscreenImage_ = VK_NULL_HANDLE;
            }
            if (offscreenMemory_ != VK_NULL_HANDLE) {
                vkFreeMemory(device_, offscreenMemory_, nullptr);
                offscreenMemory_ = VK_NULL_HANDLE;
            }
            if (swapchain_ != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
                swapchain_ = VK_NULL_HANDLE;
            }

            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
            graphicsQueue_ = VK_NULL_HANDLE;
            presentQueue_ = VK_NULL_HANDLE;
        }

        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }

        physicalDevice_ = VK_NULL_HANDLE;
        images_.clear();
        imageLayouts_.clear();
        status_ = "STOPPED";
    }

    ~VulkanRenderer() {
        stop();
    }

private:
    std::string abortStart(const std::string& message) {
        status_ = message;
        stopResourcesButKeepStatus();
        return status_;
    }

    void stopResourcesButKeepStatus() {
        const std::string saved = status_;
        stop();
        status_ = saved;
    }

    bool failFrame(const std::string& message) {
        status_ = message;
        return false;
    }

    static std::string vkResultName(VkResult result) {
        return "VkResult " + std::to_string(static_cast<int>(result));
    }

    bool choosePhysicalDevice() {
        uint32_t deviceCount = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            status_ = "ERROR: No Vulkan physical devices found.";
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not enumerate physical devices.";
            return false;
        }

        for (VkPhysicalDevice candidate : devices) {
            uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &extensionCount, extensions.data());

            bool hasSwapchain = false;
            for (const auto& extension : extensions) {
                if (std::string(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
                    hasSwapchain = true;
                    break;
                }
            }
            if (!hasSwapchain) continue;

            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &familyCount, families.data());

            for (uint32_t i = 0; i < familyCount; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;

                VkBool32 canPresent = VK_FALSE;
                result = vkGetPhysicalDeviceSurfaceSupportKHR(
                    candidate, i, surface_, &canPresent);
                if (result == VK_SUCCESS && canPresent) {
                    physicalDevice_ = candidate;
                    queueFamilyIndex_ = i;
                    timestampValidBits_ = families[i].timestampValidBits;
                    vkGetPhysicalDeviceProperties(candidate, &properties_);
                    return true;
                }
            }
        }

        status_ = "ERROR: No GPU queue supports graphics and Android presentation.";
        return false;
    }

    bool createDevice() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamilyIndex_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;

        VkResult result = vkCreateDevice(
            physicalDevice_, &deviceInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkCreateDevice failed: " + vkResultName(result);
            return false;
        }

        vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &graphicsQueue_);
        presentQueue_ = graphicsQueue_;
        return true;
    }

    bool createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice_, surface_, &capabilities);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not query surface capabilities: " + vkResultName(result);
            return false;
        }

        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            status_ = "ERROR: Surface images do not support transfer-destination usage.";
            return false;
        }

        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            status_ = "ERROR: No Vulkan surface formats are available.";
            return false;
        }

        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice_, surface_, &formatCount, formats.data());
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not retrieve Vulkan surface formats.";
            return false;
        }

        bool foundFormat = false;
        for (const auto& candidate : formats) {
            VkFormat candidateFormat = candidate.format;
            if (candidateFormat == VK_FORMAT_UNDEFINED) {
                candidateFormat = VK_FORMAT_R8G8B8A8_UNORM;
            }

            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(
                physicalDevice_, candidateFormat, &formatProperties);
            const VkFormatFeatureFlags required =
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT;

            if ((formatProperties.optimalTilingFeatures & required) == required) {
                surfaceFormat_ = candidate;
                surfaceFormat_.format = candidateFormat;
                blitFilter_ =
                    (formatProperties.optimalTilingFeatures &
                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                        ? VK_FILTER_LINEAR
                        : VK_FILTER_NEAREST;
                foundFormat = true;
                break;
            }
        }

        if (!foundFormat) {
            status_ = "ERROR: No surface format supports triangle rendering and image blit.";
            return false;
        }

        VkExtent2D chosenExtent = capabilities.currentExtent;
        if (chosenExtent.width == UINT32_MAX) {
            const uint32_t width = static_cast<uint32_t>(
                std::max(1, ANativeWindow_getWidth(window_)));
            const uint32_t height = static_cast<uint32_t>(
                std::max(1, ANativeWindow_getHeight(window_)));
            chosenExtent.width = std::clamp(
                width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            chosenExtent.height = std::clamp(
                height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        extent_ = chosenExtent;

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }

        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        const VkCompositeAlphaFlagBitsKHR alphaOptions[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        for (VkCompositeAlphaFlagBitsKHR option : alphaOptions) {
            if ((capabilities.supportedCompositeAlpha & option) != 0) {
                compositeAlpha = option;
                break;
            }
        }

        VkSwapchainCreateInfoKHR swapchainInfo{};
        swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainInfo.surface = surface_;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = surfaceFormat_.format;
        swapchainInfo.imageColorSpace = surfaceFormat_.colorSpace;
        swapchainInfo.imageExtent = extent_;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha = compositeAlpha;
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;

        result = vkCreateSwapchainKHR(
            device_, &swapchainInfo, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkCreateSwapchainKHR failed: " + vkResultName(result);
            return false;
        }

        uint32_t actualImageCount = 0;
        result = vkGetSwapchainImagesKHR(
            device_, swapchain_, &actualImageCount, nullptr);
        if (result != VK_SUCCESS || actualImageCount == 0) {
            status_ = "ERROR: Could not query swapchain images.";
            return false;
        }

        images_.resize(actualImageCount);
        result = vkGetSwapchainImagesKHR(
            device_, swapchain_, &actualImageCount, images_.data());
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not retrieve swapchain images.";
            return false;
        }
        images_.resize(actualImageCount);
        imageLayouts_.assign(actualImageCount, VK_IMAGE_LAYOUT_UNDEFINED);
        renderFinished_.assign(actualImageCount, VK_NULL_HANDLE);
        return true;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags desired) {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeBits & (1U << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & desired) == desired) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool createOffscreenImage() {
        const uint32_t divisor = halfResolution_ ? 2U : 1U;
        offscreenExtent_.width = std::max(1U, (extent_.width + divisor - 1U) / divisor);
        offscreenExtent_.height = std::max(1U, (extent_.height + divisor - 1U) / divisor);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = surfaceFormat_.format;
        imageInfo.extent = {offscreenExtent_.width, offscreenExtent_.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &offscreenImage_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating offscreen GPU image failed: " + vkResultName(result);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, offscreenImage_, &requirements);

        uint32_t memoryType = findMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX) {
            memoryType = findMemoryType(requirements.memoryTypeBits, 0);
        }
        if (memoryType == UINT32_MAX) {
            status_ = "ERROR: No compatible Vulkan memory type for offscreen image.";
            return false;
        }

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize = requirements.size;
        allocationInfo.memoryTypeIndex = memoryType;

        result = vkAllocateMemory(device_, &allocationInfo, nullptr, &offscreenMemory_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Allocating offscreen image memory failed: " + vkResultName(result);
            return false;
        }

        result = vkBindImageMemory(device_, offscreenImage_, offscreenMemory_, 0);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Binding offscreen image memory failed: " + vkResultName(result);
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = offscreenImage_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = surfaceFormat_.format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        result = vkCreateImageView(device_, &viewInfo, nullptr, &imageView_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating offscreen image view failed: " + vkResultName(result);
            return false;
        }
        return true;
    }

    bool createRenderPassAndFramebuffer() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = surfaceFormat_.format;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;

        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        VkResult result = vkCreateRenderPass(
            device_, &renderPassInfo, nullptr, &renderPass_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating render pass failed: " + vkResultName(result);
            return false;
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &imageView_;
        framebufferInfo.width = offscreenExtent_.width;
        framebufferInfo.height = offscreenExtent_.height;
        framebufferInfo.layers = 1;

        result = vkCreateFramebuffer(
            device_, &framebufferInfo, nullptr, &framebuffer_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating offscreen framebuffer failed: " + vkResultName(result);
            return false;
        }
        return true;
    }

    bool readShaderAsset(
            AAssetManager* manager,
            const char* path,
            std::vector<uint32_t>& words) {
        AAsset* asset = AAssetManager_open(manager, path, AASSET_MODE_BUFFER);
        if (asset == nullptr) {
            status_ = std::string("ERROR: Could not open shader asset: ") + path;
            return false;
        }

        const off_t length = AAsset_getLength(asset);
        if (length <= 0 || (length % 4) != 0) {
            AAsset_close(asset);
            status_ = std::string("ERROR: Invalid SPIR-V shader length: ") + path;
            return false;
        }

        words.resize(static_cast<size_t>(length) / sizeof(uint32_t));
        const int bytesRead = AAsset_read(asset, words.data(), static_cast<size_t>(length));
        AAsset_close(asset);

        if (bytesRead != length) {
            status_ = std::string("ERROR: Could not read complete SPIR-V shader: ") + path;
            return false;
        }
        return true;
    }

    bool createShaderModule(
            const std::vector<uint32_t>& words,
            VkShaderModule* module) {
        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderInfo.codeSize = words.size() * sizeof(uint32_t);
        shaderInfo.pCode = words.data();

        VkResult result = vkCreateShaderModule(
            device_, &shaderInfo, nullptr, module);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating shader module failed: " + vkResultName(result);
            return false;
        }
        return true;
    }

    bool createGraphicsPipeline(AAssetManager* assetManager) {
        std::vector<uint32_t> vertexWords;
        std::vector<uint32_t> fragmentWords;

        if (!readShaderAsset(assetManager, "shaders/triangle.vert.spv", vertexWords)) {
            return false;
        }
        if (!readShaderAsset(assetManager, "shaders/triangle.frag.spv", fragmentWords)) {
            return false;
        }

        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;

        if (!createShaderModule(vertexWords, &vertexModule)) return false;
        if (!createShaderModule(fragmentWords, &fragmentModule)) {
            vkDestroyShaderModule(device_, vertexModule, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";

        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(offscreenExtent_.width);
        viewport.height = static_cast<float>(offscreenExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = offscreenExtent_;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampling.sampleShadingEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkPushConstantRange scenePushRange{};
        scenePushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        scenePushRange.offset = 0;
        scenePushRange.size = sizeof(float) * 3;

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &scenePushRange;

        VkResult result = vkCreatePipelineLayout(
            device_, &layoutInfo, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device_, fragmentModule, nullptr);
            vkDestroyShaderModule(device_, vertexModule, nullptr);
            status_ = "ERROR: Creating graphics pipeline layout failed: " + vkResultName(result);
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineIndex = -1;

        result = vkCreateGraphicsPipelines(
            device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_);

        vkDestroyShaderModule(device_, fragmentModule, nullptr);
        vkDestroyShaderModule(device_, vertexModule, nullptr);

        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating graphics pipeline failed: " + vkResultName(result);
            return false;
        }
        return true;
    }

    bool createFrameResources() {
        if (timestampValidBits_ > 0) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 2;
            // Timing is optional: unsupported queues can still render the scene.
            vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampQueryPool_);
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex_;

        VkResult result = vkCreateCommandPool(
            device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkCreateCommandPool failed: " + vkResultName(result);
            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device_, &commandInfo, &commandBuffer_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkAllocateCommandBuffers failed: " + vkResultName(result);
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = vkCreateSemaphore(
            device_, &semaphoreInfo, nullptr, &imageAvailable_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not create image-available semaphore: " + vkResultName(result);
            return false;
        }

        for (auto& semaphore : renderFinished_) {
            result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) {
                status_ = "ERROR: Could not create render-finished semaphore: " + vkResultName(result);
                return false;
            }
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(
            device_, &fenceInfo, nullptr, &inFlightFence_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not create frame fence: " + vkResultName(result);
            return false;
        }
        return true;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    ANativeWindow* window_ = nullptr;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    uint32_t queueFamilyIndex_ = 0;
    uint32_t timestampValidBits_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSurfaceFormatKHR surfaceFormat_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageLayout> imageLayouts_;

    VkImage offscreenImage_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkExtent2D offscreenExtent_{};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
    VkFilter blitFilter_ = VK_FILTER_NEAREST;
    bool halfResolution_ = true;
    uint32_t lightingMode_ = 2;
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    float latestGpuMs_ = -1.0f;

    std::vector<VkSemaphore> renderFinished_;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence inFlightFence_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;

    uint64_t frameNumber_ = 0;
    std::string status_ = "STOPPED";
};

static std::unique_ptr<VulkanRenderer> gRenderer;

extern "C" JNIEXPORT jstring JNICALL
Java_com_devdgames_sixsevenframes_MainActivity_nativeStart(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface,
    jobject assetManager,
    jboolean halfResolution,
    jint lightingMode) {
    gRenderer = std::make_unique<VulkanRenderer>();
    const std::string status = gRenderer->start(
        env, surface, assetManager, halfResolution == JNI_TRUE,
        static_cast<uint32_t>(std::max(0, lightingMode)));
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_devdgames_sixsevenframes_MainActivity_nativeDrawFrame(
    JNIEnv* /* env */, jobject /* thiz */) {
    if (!gRenderer) return JNI_FALSE;
    return gRenderer->drawFrame() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_devdgames_sixsevenframes_MainActivity_nativeLastStatus(
    JNIEnv* env, jobject /* thiz */) {
    const std::string status = gRenderer
        ? gRenderer->lastStatus()
        : "ERROR: Vulkan renderer is unavailable.";
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_devdgames_sixsevenframes_MainActivity_nativeStop(
    JNIEnv* /* env */, jobject /* thiz */) {
    if (gRenderer) {
        gRenderer->stop();
        gRenderer.reset();
    }
}
