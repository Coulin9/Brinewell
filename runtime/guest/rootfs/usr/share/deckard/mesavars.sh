# SPDX-License-Identifier: MIT
# Derived from Valve Corporation's deckard-steamvr-session (MIT)
# brinewell: replaces the Frame's /usr/share/deckard/mesavars.sh
# (EnvironmentFile of gamescope-session.service and steam.service).
# The Brinewell layer provides Venus (libvulkan_virtio.so) and Zink from the
# official Mesa release. Venus forwards Vulkan commands over virtio-gpu to
# KosmicKrisp in the macOS VM host. Zink implements GL on that Vulkan device.
#
# Dropped from stock: VRCOMPOSITOR_TU_DEBUG (Turnip-only, for the SteamVR
# compositor, which is masked in the VM). No VK_ICD_FILENAMES/VK_DRIVER_FILES:
# Turnip finds no kgsl/msm device and is skipped, and a global value would leak
# into pressure-vessel/FEX where the aarch64 JSON is wrong.

# zink's native GL version follows the host Vulkan features Venus passes through:
# KosmicKrisp reaches 4.6 core/compat, MoltenVK lacks features and reports less.
# The override must not cap the version below 4.6: Prism3D games (American Truck
# Simulator, ETS2) request a 4.6 core context with no fallback and exit on the
# resulting GLXBadFBConfig.
MESA_GL_VERSION_OVERRIDE=4.6
MESA_GLSL_VERSION_OVERRIDE=460

# GL through zink (the Frame image ships no virgl/virtio_gpu GL driver)
MESA_LOADER_DRIVER_OVERRIDE=zink
