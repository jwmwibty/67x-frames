# 67xFrames

An Android-native Vulkan graphics research project exploring lower-cost rendering and spatial reconstruction.

## Current benchmark scene

- A full-screen procedural 3D tunnel generated in a fragment shader; no external model or texture files are required.
- Ray-marched signed-distance geometry with repeating arches, pillars, emissive strips, soft shadows, ambient occlusion, specular lighting, fog, and camera motion.
- A one-tap switch between native-resolution and half-resolution rendering. Half resolution is upscaled to the Android surface using Vulkan image blitting.
- Vulkan timestamp-query reporting for the offscreen scene pass when the selected GPU queue supports timestamps.
- Kotlin Android activity, native C++ renderer, and GitHub Actions debug APK build.

The scene is a deliberately shader-heavy workload, not a production game or a system-wide game interceptor. GPU timings measure the offscreen scene pass; they do not include the final blit, Android composition, or display scanout.

## Build

Push to main or run Actions → Android CI → Run workflow. The workflow compiles the GLSL shaders, builds a debug APK, and uploads it as the 67xFrames-debug-apk artifact.
