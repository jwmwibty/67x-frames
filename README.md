# 67xFrames

Android-native Vulkan graphics research project exploring lower-cost rendering and spatial reconstruction.

## Stable procedural benchmark

- Animated tunnel made from analytic floor, ceiling, side-wall, and arch intersections.
- No ray-marching loop, soft-shadow loop, ambient-occlusion loop, external model, or texture files.
- OFF / PARTIAL / MAX lighting presets. MAX adds a small specular calculation only.
- Half-resolution rendering by default, with an option to switch to native resolution.
- Rolling 10-second average FPS and 1% low FPS from frame-time samples.
- Vulkan timestamp queries are disabled for broader mobile-driver compatibility; FPS measurement remains available.

The initial preset is OFF to keep startup light on older GPUs. Cycle OFF → PARTIAL → MAX only after checking stability. The FPS measurement reflects the app's rendered-frame cadence, including waits on the GPU/presentation path.

## Build

Push to main or run **Actions → Android CI → Run workflow**. The workflow compiles the GLSL shaders, builds a debug APK, and uploads it as the `67xFrames-debug-apk` artifact.
