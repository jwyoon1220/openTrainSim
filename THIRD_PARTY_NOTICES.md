# Third-party software

openTrainSim is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
It uses the following third-party components, each under its own license. Components marked
"fetched" are downloaded at configure time and are not part of this repository.

| Component | Version | Use | License |
|---|---|---|---|
| [GLFW](https://www.glfw.org/) | 3.4 (system or fetched) | window, input | zlib |
| [volk](https://github.com/zeux/volk) | 1.4.304 (fetched) | Vulkan function loader | MIT |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.3.0 (fetched) | GPU memory | MIT |
| [Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers) | system / Vulkan SDK, or 1.4.304 fetched | API | Apache-2.0 / MIT |
| [GLM](https://github.com/g-truc/glm) | system, or 1.0.3 fetched | math | MIT |
| [stb](https://github.com/nothings/stb) (stb_image, stb_dxt, stb_easy_font) | fetched | image decoding, BC compression, HUD font | MIT or public domain |
| [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | 1.0.2 (fetched) | FSR upscaling | MIT |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | 310.9.16 (fetched, optional: `PHX_DLSS`) | DLSS, frame generation | NVIDIA RTX SDK license (proprietary) |
| [OpenAL Soft](https://openal-soft.org/) | system, or 1.25.2 fetched (Windows: OpenAL32.dll); dynamically linked | sound output | LGPL-2.0-or-later |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.14 (vendored: `third_party/cgltf`) | glTF 2.0 models (3D cabs) | MIT |
| iconv (glibc) | system (Linux; Windows uses its own code page API) | legacy code page decoding | LGPL-2.1-or-later |
| [Mono](https://www.mono-project.com/) | system (Linux: build + run time; Windows uses the .NET Framework) | train plugin host | MIT |
| OpenBveApi.dll ([openBVE](https://github.com/leezer3/OpenBVE)) | from an openBVE installation | train plugin API | public domain / BSD-2-Clause |

## Notes

- **openBVE.** Route, object, train, sound and plugin behaviour follow openBVE's documented
  formats and its source code, re-implemented in C++. openBVE was placed in the public domain by
  its original author; newer openBVE code is BSD-2-Clause.
- **NVIDIA DLSS SDK.** Proprietary. Its license allows shipping the runtime libraries with an
  application, limits DLSS to systems with NVIDIA GPUs, and requires notifying NVIDIA before a
  commercial release (https://developer.nvidia.com/sw-notification). Building with
  `-DPHX_DLSS=OFF` removes it entirely; FSR / TAA are used instead.
- **OpenAL Soft** is LGPL: keep it dynamically linked; if you ship its library, include its
  license and allow users to replace it.
- **Content.** Routes, trains and plugins (e.g. the `data/` add-on) belong to their authors and
  are not covered by this project's license.
