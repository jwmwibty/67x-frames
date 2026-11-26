#define VK_USE_PLATFORM_ANDROID_KHR

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
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
            return fail("ERROR: Could not obtain ANativeWindow from Surface.");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "67xFrames";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
        appInfo.pEngineName = "67xFrames";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
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
            return fail("ERROR: vkCreateInstance failed: " + vkResultName(result));
        }

        VkAndroidSurfaceCreateInfoKHR androidSurfaceInfo{};
        androidSurfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        androidSurfaceInfo.window = window_;

        result = vkCreateAndroidSurfaceKHR(
            instance_, &androidSurfaceInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            return fail("ERROR: vkCreateAndroidSurfaceKHR failed: " + vkResultName(result));
        }

        if (!choosePhysicalDevice()) {
            return fail("ERROR: No GPU queue supports both graphics and presentation.");
        }

        if (!createDevice()) {
            return fail(status_);
        }

        if (!createSwapchain()) {
            return fail(status_);
        }

        if (!createFrameResources()) {
            return fail(status_);
        }

        status_ = "READY\n67xFrames — Vulkan frame renderer\n";
        status_ += "GPU: ";
        status_ += properties_.deviceName;
        status_ += "\nDevice API: ";
        status_ += std::to_string(VK_VERSION_MAJOR(properties_.apiVersion));
        status_ += ".";
        status_ += std::to_string(VK_VERSION_MINOR(properties_.apiVersion));
        status_ += ".";
        status_ += std::to_string(VK_VERSION_PATCH(properties_.apiVersion));
        status_ += "\nSwapchain: ";
        status_ += std::to_string(extent_.width);
        status_ += " x ";
        status_ += std::to_string(extent_.height);
        status_ += "\nSwapchain images: ";
        status_ += std::to_string(images_.size());
        status_ += "\nVulkan clear + present: running";
        return status_;
    }

    bool drawFrame() {
        if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE) {
            status_ = "ERROR: Renderer is not initialized.";
            return false;
        }

        VkResult result = vkWaitForFences(
            device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Waiting for frame fence failed: " + vkResultName(result);
            return false;
        }

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(
            device_, swapchain_, 1000000000ULL,
            imageAvailable_, VK_NULL_HANDLE, &imageIndex);

        if (result == VK_TIMEOUT || result == VK_NOT_READY) {
            return true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            status_ = "ERROR: Swapchain is out of date. Reopen the app to recreate it.";
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            status_ = "ERROR: vkAcquireNextImageKHR failed: " + vkResultName(result);
            return false;
        }

        result = vkResetCommandBuffer(commandBuffer_, 0);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkResetCommandBuffer failed: " + vkResultName(result);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkBeginCommandBuffer failed: " + vkResultName(result);
            return false;
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = imageLayouts_[imageIndex];
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = images_[imageIndex];
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = 0;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount = 1;

        const VkPipelineStageFlags sourceStage =
            imageLayouts_[imageIndex] == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        vkCmdPipelineBarrier(
            commandBuffer_,
            sourceStage,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.025f;
        clearColor.float32[1] = 0.090f;
        clearColor.float32[2] = 0.145f;
        clearColor.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(
            commandBuffer_,
            images_[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clearColor,
            1,
            &range);

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
            status_ = "ERROR: vkEndCommandBuffer failed: " + vkResultName(result);
            return false;
        }

        result = vkResetFences(device_, 1, &inFlightFence_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkResetFences failed: " + vkResultName(result);
            return false;
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
            status_ = "ERROR: vkQueueSubmit failed: " + vkResultName(result);
            return false;
        }
        imageLayouts_[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished_[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            status_ = "ERROR: Swapchain changed size/state. Reopen the app to recreate it.";
            return false;
        }
        if (result != VK_SUCCESS) {
            status_ = "ERROR: vkQueuePresentKHR failed: " + vkResultName(result);
            return false;
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
    std::string fail(const std::string& message) {
        status_ = message;
        stopResourcesButKeepStatus();
        return status_;
    }

    void stopResourcesButKeepStatus() {
        const std::string saved = status_;
        stop();
        status_ = saved;
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
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);

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
                    properties_ = properties;
                    return true;
                }
            }
        }

        status_ = "ERROR: No graphics queue can present to this Android surface.";
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

        VkSurfaceFormatKHR chosenFormat = formats[0];
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosenFormat = format;
                break;
            }
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
        swapchainInfo.imageFormat = chosenFormat.format;
        swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
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
            status_ = "ERROR: Could not create image-available semaphore.";
            return false;
        }

        for (auto& semaphore : renderFinished_) {
            result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
            if (result != VK_SUCCESS) {
                status_ = "ERROR: Could not create render-finished semaphores.";
                return false;
            }
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFence_);
        if (result != VK_SUCCESS) {
            status_ = "ERROR: Could not create frame fence.";
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
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageLayout> imageLayouts_;
    std::vector<VkSemaphore> renderFinished_;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkFence inFlightFence_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
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
