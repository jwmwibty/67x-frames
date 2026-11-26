#define VK_USE_PLATFORM_ANDROID_KHR

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VulkanRenderer {
public:
    std::string start(JNIEnv* env, jobject surfaceObject) {
        stop();

        window_ = ANativeWindow_fromSurface(env, surfaceObject);
        if (window_ == nullptr) {
            return abortStart("ERROR: Could not obtain ANativeWindow from Surface.");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "67xFrames";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
        appInfo.pEngineName = "67xFrames";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 3, 0);
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

        if (!choosePhysicalDevice()) {
            return abortStart(status_);
        }
        if (!createDevice()) {
            return abortStart(status_);
        }
        if (!createSwapchain()) {
            return abortStart(status_);
        }
        if (!createOffscreenImage()) {
            return abortStart(status_);
        }
        if (!createFrameResources()) {
            return abortStart(status_);
        }

        status_ = "READY\n67xFrames — offscreen Vulkan renderer\n";
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
        status_ += "\nUpscale filter: ";
        status_ += (blitFilter_ == VK_FILTER_LINEAR ? "linear blit" : "nearest blit");
        status_ += "\nSwapchain images: ";
        status_ += std::to_string(images_.size());
        status_ += "\nGPU clear → blit → present: running";
        return status_;
    }

    bool drawFrame() {
        if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE ||
            offscreenImage_ == VK_NULL_HANDLE) {
            status_ = "ERROR: Renderer is not initialized.";
            return false;
        }

        VkResult result = vkWaitForFences(
            device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            return failFrame("ERROR: Waiting for frame fence failed: " + vkResultName(result));
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

        // Reuse the smaller GPU image. It starts undefined, then remains in
        // TRANSFER_SRC_OPTIMAL after each frame so it can be cleared again.
        VkImageMemoryBarrier offscreenToDst{};
        offscreenToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        offscreenToDst.srcAccessMask =
            offscreenInitialized_ ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        offscreenToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        offscreenToDst.oldLayout = offscreenInitialized_
            ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        offscreenToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        offscreenToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offscreenToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offscreenToDst.image = offscreenImage_;
        offscreenToDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        offscreenToDst.subresourceRange.baseMipLevel = 0;
        offscreenToDst.subresourceRange.levelCount = 1;
        offscreenToDst.subresourceRange.baseArrayLayer = 0;
        offscreenToDst.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            commandBuffer_,
            offscreenInitialized_ ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &offscreenToDst);

        // The acquired swapchain image is either first-use UNDEFINED or
        // PRESENT_SRC_KHR after the presentation engine released it.
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

        // Animated clear color makes it visible that new frames are being
        // produced in the half-resolution image before they are upscaled.
        const float phase = static_cast<float>(frameNumber_ % 360U) *
                            0.01745329251994329577f;
        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.08f + 0.07f * (0.5f + 0.5f * std::sin(phase));
        clearColor.float32[1] = 0.10f + 0.08f * (0.5f + 0.5f * std::sin(phase + 2.1f));
        clearColor.float32[2] = 0.16f + 0.10f * (0.5f + 0.5f * std::sin(phase + 4.2f));
        clearColor.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(
            commandBuffer_,
            offscreenImage_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor,
            1,
            &range);

        VkImageMemoryBarrier offscreenToSrc{};
        offscreenToSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        offscreenToSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        offscreenToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        offscreenToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        offscreenToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        offscreenToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offscreenToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offscreenToSrc.image = offscreenImage_;
        offscreenToSrc.subresourceRange = range;

        vkCmdPipelineBarrier(
            commandBuffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &offscreenToSrc);

        // Scale the smaller GPU image to the full swapchain extent.
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
        toPresent.subresourceRange = range;

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

        offscreenInitialized_ = true;
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

    const std::string& lastStatus() const {
        return status_;
    }

    void stop() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);

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
        offscreenInitialized_ = false;
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
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
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
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

            for (uint32_t i = 0; i < familyCount; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    continue;
                }

                VkBool32 canPresent = VK_FALSE;
                result = vkGetPhysicalDeviceSurfaceSupportKHR(
                    candidate, i, surface_, &canPresent);
                if (result == VK_SUCCESS && canPresent) {
                    physicalDevice_ = candidate;
                    queueFamilyIndex_ = i;
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
                VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

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
            status_ = "ERROR: No surface format supports Vulkan blit source and destination.";
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
        offscreenExtent_.width = std::max(1U, (extent_.width + 1U) / 2U);
        offscreenExtent_.height = std::max(1U, (extent_.height + 1U) / 2U);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = surfaceFormat_.format;
        imageInfo.extent = {offscreenExtent_.width, offscreenExtent_.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &offscreenImage_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Creating half-resolution GPU image failed: " + vkResultName(result);
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
        return true;
    }

    bool createFrameResources() {
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
        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_);
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
        result = vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFence_);
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
    VkExtent2D offscreenExtent_{};
    bool offscreenInitialized_ = false;
    VkFilter blitFilter_ = VK_FILTER_NEAREST;

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
    JNIEnv* env, jobject /* thiz */, jobject surface) {
    gRenderer = std::make_unique<VulkanRenderer>();
    const std::string status = gRenderer->start(env, surface);
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
