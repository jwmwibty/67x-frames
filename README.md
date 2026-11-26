# 67xFrames

An Android-native graphics research project exploring lower-cost rendering and spatial reconstruction.

## Current milestone

- Kotlin Android activity
- Native C++ library built with the Android NDK and CMake
- Vulkan loader linked into the native library
- Runtime Vulkan instance/device probe (GPU name and Vulkan API version)
- GitHub Actions debug APK build

This is **not yet** a system-wide game interceptor or a Vulkan layer. The first goal is to verify the app build and Vulkan access from a native Android process. Rendering, swapchain/surface handling, interception, and upscaling will be added in later milestones.

## Build

Push to `main` or run **Actions → Android CI → Run workflow**. The workflow builds a debug APK and uploads it as the `67xFrames-debug-apk` artifact.
