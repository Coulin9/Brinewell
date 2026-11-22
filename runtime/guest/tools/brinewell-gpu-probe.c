// Brinewell: read virtio-gpu features and the Neptune capset from the guest.
//
// Exit codes: 0 = Neptune capset present, 1 = no Neptune capset, 2 = no device.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/virtgpu_drm.h>

#define NPT_CAPSET_ID 7

struct npt_capset {
    uint32_t wire_format_version;
    uint32_t caps_flags;
    uint32_t pad[13];
};

static int open_render_node(void) {
    for (int i = 128; i < 136; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

static int create_context(int fd) {
    struct drm_virtgpu_context_set_param params[3] = {
        { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, .value = NPT_CAPSET_ID },
        { .param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS, .value = 1 },
        { .param = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME, .value = (uintptr_t)"brinewell-gpu-probe" },
    };
    struct drm_virtgpu_context_init ci = {
        .num_params = 3,
        .ctx_set_params = (uintptr_t)params,
    };
    return ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci);
}

int main(int argc, char **argv) {
    int want_context = argc > 1 && strcmp(argv[1], "--context") == 0;
    int fd = open_render_node();
    if (fd < 0) {
        fprintf(stderr, "brinewell-gpu-probe: no /dev/dri/renderD*: %s\n", strerror(errno));
        return 2;
    }
    struct {
        uint64_t param;
        uint64_t value;
    } gp = {0};
    static const struct {
        uint64_t id;
        const char *name;
    } params[] = {
        {VIRTGPU_PARAM_3D_FEATURES, "3d_features"},
        {VIRTGPU_PARAM_CAPSET_QUERY_FIX, "capset_query_fix"},
        {VIRTGPU_PARAM_RESOURCE_BLOB, "resource_blob"},
        {VIRTGPU_PARAM_HOST_VISIBLE, "host_visible"},
        {VIRTGPU_PARAM_CONTEXT_INIT, "context_init"},
        {9, "blob_alignment"},
    };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        uint64_t value = 0;
        gp.param = params[i].id;
        gp.value = (uint64_t)(uintptr_t)&value;
        if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) == 0)
            printf("param %s=%llu\n", params[i].name, (unsigned long long)value);
        else
            printf("param %s=unsupported\n", params[i].name);
    }

    struct npt_capset capset;
    memset(&capset, 0, sizeof(capset));
    struct drm_virtgpu_get_caps gc = {
        .cap_set_id = NPT_CAPSET_ID,
        .cap_set_ver = 0,
        .addr = (uintptr_t)&capset,
        .size = sizeof(capset),
    };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc) != 0) {
        printf("capset neptune=absent errno=%d\n", errno);
        close(fd);
        return 1;
    }
    printf("capset neptune wire=0x%08x caps=0x%08x d3d12=%u\n",
           capset.wire_format_version, capset.caps_flags,
           (capset.caps_flags & 1u) ? 1u : 0u);
    if (want_context && capset.wire_format_version) {
        if (create_context(fd) == 0) {
            printf("context neptune=created\n");
            sleep(5);
        } else {
            printf("context neptune=failed errno=%d\n", errno);
        }
    }
    close(fd);
    return capset.wire_format_version ? 0 : 1;
}
