# 67xFrames

An Android-native Vulkan graphics research project exploring lower-cost rendering and spatial reconstruction.

## Procedural benchmark

- Full-screen animated 3D tunnel generated in GLSL; no external models or texture archives.
- Ray-marched arches, pillars, floor panels, emissive strips, and repeating ceiling lamps.
- Lighting presets: OFF (safe startup, unlit but visible geometry), PARTIAL (ambient, diffuse and point lights), MAX (reduced-cost soft shadows, ambient occlusion, specular highlights and a deeper ray-march budget).
- Native-resolution and half-resolution rendering switch. Half resolution is upscaled with a Vulkan image blit.
- Live 10-second average FPS and 1% low FPS, calculated from frame-time samples.
- Vulkan timestamp query for the offscreen scene-render pass when supported by the GPU queue.

The app starts in OFF mode to avoid stressing older mobile GPUs during launch; tap the lighting button to cycle OFF → PARTIAL → MAX. The FPS values describe the app's rendered-frame cadence, including waits for the GPU/presentation path. The Vulkan timestamp describes the offscreen scene pass only; it excludes the final blit, Android composition, and display scanout.

## Build

Push to main or run **Actions → Android CI → Run workflow**. The workflow compiles the GLSL shaders, builds a debug APK, and uploads it as the `67xFrames-debug-apk` artifact.
