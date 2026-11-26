#include <jni.h>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

extern "C" JNIEXPORT jstring JNICALL
Java_com_devdgames_sixsevenframes_MainActivity_nativeStatus(
        JNIEnv* env,
        jobject /* thiz */) {

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "67xFrames";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "67xFrames";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);

    if (result != VK_SUCCESS) {
        const std::string message =
                "67xFrames\nVulkan instance creation failed. VkResult: "
                + std::to_string(static_cast<int>(result));
        return env->NewStringUTF(message.c_str());
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (result != VK_SUCCESS || deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(
                "67xFrames\nVulkan instance created, but no physical GPU was found.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(
            instance, &deviceCount, devices.data());

    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return env->NewStringUTF(
                "67xFrames\nCould not enumerate Vulkan physical devices.");
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices[0], &properties);

    const uint32_t major = VK_VERSION_MAJOR(properties.apiVersion);
    const uint32_t minor = VK_VERSION_MINOR(properties.apiVersion);
    const uint32_t patch = VK_VERSION_PATCH(properties.apiVersion);

    std::string message = "67xFrames\n";
    message += "Vulkan instance: OK\n";
    message += "GPU: ";
    message += properties.deviceName;
    message += "\nVulkan device API: ";
    message += std::to_string(major) + "."
            + std::to_string(minor) + "."
            + std::to_string(patch);
    message += "\n\nMilestone 1: native Vulkan access";

    vkDestroyInstance(instance, nullptr);
    return env->NewStringUTF(message.c_str());
}
