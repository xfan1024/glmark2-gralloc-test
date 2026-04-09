#include "gralloc_egl_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#include <gbm.h>

/* EGL function pointer types (avoid direct EGL header inclusion) */
typedef EGLImage (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, void *ctx, unsigned int target, void *buffer, const int *attrib_list);
typedef int (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImage image);

enum gralloc_backend_type {
    GRALLOC_BACKEND_NONE = 0,
    GRALLOC_BACKEND_GBM,
    GRALLOC_BACKEND_DUMB
};

struct gralloc_platform {
    enum gralloc_backend_type backend;
    int devfd;
    struct gbm_device *gbm;
    GRALLOC_EGLGETPROCADDRESS get_proc_address;
};

static struct gralloc_platform g_gralloc = {};

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = NULL;

static void die_errno(const char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

static void die_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

static void init_egl_procs(void)
{
    if (!g_gralloc.get_proc_address) {
        die_msg("gralloc_init() not called with get_proc_address callback");
    }

    p_eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)g_gralloc.get_proc_address("eglCreateImageKHR");
    p_eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)g_gralloc.get_proc_address("eglDestroyImageKHR");

    if (!p_eglCreateImageKHR) {
        die_msg("get_proc_address(\"eglCreateImageKHR\") failed");
    }

    if (!p_eglDestroyImageKHR) {
        die_msg("get_proc_address(\"eglDestroyImageKHR\") failed");
    }
}

static int open_dev(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        die_errno(path);
    }
    return fd;
}

static unsigned int gbm_usage_flag_from_token(const char *token)
{
    if (strcmp(token, "scanout") == 0) {
        return GBM_BO_USE_SCANOUT;
    }
    if (strcmp(token, "cursor") == 0) {
        return GBM_BO_USE_CURSOR;
    }
    if (strcmp(token, "rendering") == 0) {
        return GBM_BO_USE_RENDERING;
    }
    if (strcmp(token, "write") == 0) {
        return GBM_BO_USE_WRITE;
    }
    if (strcmp(token, "linear") == 0) {
        return GBM_BO_USE_LINEAR;
    }

    fprintf(stderr, "unknown GBM_USAGE token: %s\n", token);
    exit(1);
}

static unsigned int gbm_usage_from_env(void)
{
    const char *env = getenv("GBM_USAGE");
    unsigned int usage = 0;

    if (!env || !env[0]) {
        return GBM_BO_USE_RENDERING;
    }

    char *copy = strdup(env);
    if (!copy) {
        die_errno("strdup");
    }

    char *saveptr = NULL;
    char *tok = strtok_r(copy, ",", &saveptr);
    while (tok) {
        usage |= gbm_usage_flag_from_token(tok);
        tok = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);

    if (usage == 0) {
        die_msg("GBM_USAGE parsed to zero");
    }

    return usage;
}

static void destroy_dumb_handle(uint32_t handle)
{
    struct drm_mode_destroy_dumb destroy_req = {};
    destroy_req.handle = handle;

    if (drmIoctl(g_gralloc.devfd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req) < 0) {
        die_errno("DRM_IOCTL_MODE_DESTROY_DUMB");
    }
}

static struct gralloc_image alloc_image_from_gbm(EGLDisplay dpy, int width, int height)
{
    struct gralloc_image out = {};
    struct gbm_bo *bo = NULL;
    int dma_fd = -1;
    uint32_t stride = 0;

    fprintf(stderr, "[Gralloc_GBM] Allocating GBM buffer (%dx%d)\n", width, height);
    
    bo = gbm_bo_create(g_gralloc.gbm,
                       (uint32_t)width,
                       (uint32_t)height,
                       GBM_FORMAT_XRGB8888,
                       gbm_usage_from_env());
    if (!bo) {
        fprintf(stderr, "[Gralloc_GBM] ERROR: gbm_bo_create failed\n");
        die_msg("gbm_bo_create failed");
    }
    fprintf(stderr, "[Gralloc_GBM] GBM buffer object created\n");

    dma_fd = gbm_bo_get_fd(bo);
    if (dma_fd < 0) {
        fprintf(stderr, "[Gralloc_GBM] ERROR: gbm_bo_get_fd failed\n");
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_fd failed");
    }
    fprintf(stderr, "[Gralloc_GBM] Got DMA-BUF FD: %d\n", dma_fd);

    stride = gbm_bo_get_stride(bo);
    if (stride == 0) {
        fprintf(stderr, "[Gralloc_GBM] ERROR: gbm_bo_get_stride returned 0\n");
        close(dma_fd);
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_stride returned 0");
    }
    fprintf(stderr, "[Gralloc_GBM] Buffer stride: %u bytes\n", stride);
    uint32_t offset = gbm_bo_get_offset(bo, 0);
    uint64_t mod = gbm_bo_get_modifier(bo);
    EGLint attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE
    };

    fprintf(stderr, "[Gralloc_GBM] Creating EGLImage from DMA-BUF\n");
    out.image = p_eglCreateImageKHR(dpy,
                                    EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT,
                                    NULL,
                                    attrs);

    close(dma_fd);

    if (out.image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[Gralloc_GBM] ERROR: eglCreateImageKHR failed\n");
        gbm_bo_destroy(bo);
        die_msg("eglCreateImageKHR failed for GBM path");
    }
    fprintf(stderr, "[Gralloc_GBM] EGLImage created successfully: %p\n", out.image);

    out.priv = (void *)bo;
    return out;
}

static struct gralloc_image alloc_image_from_dumb(EGLDisplay dpy, int width, int height)
{
    struct gralloc_image out = {};
    struct drm_mode_create_dumb create_req = {};
    int dma_fd = -1;

    create_req.width = (uint32_t)width;
    create_req.height = (uint32_t)height;
    create_req.bpp = 32;

    if (drmIoctl(g_gralloc.devfd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
        die_errno("DRM_IOCTL_MODE_CREATE_DUMB");
    }

    if (create_req.pitch == 0) {
        destroy_dumb_handle(create_req.handle);
        die_msg("DRM_IOCTL_MODE_CREATE_DUMB returned zero pitch");
    }

    if (drmPrimeHandleToFD(g_gralloc.devfd, create_req.handle, DRM_CLOEXEC, &dma_fd) != 0) {
        destroy_dumb_handle(create_req.handle);
        die_errno("drmPrimeHandleToFD");
    }

    uint64_t mod = DRM_FORMAT_MOD_LINEAR;
    EGLint attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)create_req.pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE
    };

    out.image = p_eglCreateImageKHR(dpy,
                                    EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT,
                                    NULL,
                                    attrs);

    close(dma_fd);

    if (out.image == EGL_NO_IMAGE_KHR) {
        destroy_dumb_handle(create_req.handle);
        die_msg("eglCreateImageKHR failed for dumb path");
    }

    out.priv = (void *)(uintptr_t)create_req.handle;
    return out;
}

void gralloc_init(GRALLOC_EGLGETPROCADDRESS get_proc_address)
{
    const char *gbm_dev = getenv("GBM_DEV");
    const char *dumb_dev = getenv("DUMB_DEV");

    fprintf(stderr, "[Gralloc_Init] Starting gralloc initialization\n");
    fprintf(stderr, "[Gralloc_Init] GBM_DEV=%s, DUMB_DEV=%s\n", 
            gbm_dev ? gbm_dev : "(not set)", 
            dumb_dev ? dumb_dev : "(not set)");

    g_gralloc.backend = GRALLOC_BACKEND_NONE;
    g_gralloc.devfd = -1;
    g_gralloc.gbm = NULL;
    g_gralloc.get_proc_address = get_proc_address;

    init_egl_procs();
    fprintf(stderr, "[Gralloc_Init] EGL procs initialized\n");

    if (gbm_dev && gbm_dev[0] && dumb_dev && dumb_dev[0]) {
        die_msg("GBM_DEV and DUMB_DEV are both set; exactly one must be set");
    }

    if ((!gbm_dev || !gbm_dev[0]) && (!dumb_dev || !dumb_dev[0])) {
        die_msg("Neither GBM_DEV nor DUMB_DEV is set; exactly one must be set");
    }

    if (gbm_dev && gbm_dev[0]) {
        fprintf(stderr, "[Gralloc_Init] Using GBM backend with device: %s\n", gbm_dev);
        g_gralloc.backend = GRALLOC_BACKEND_GBM;
        g_gralloc.devfd = open_dev(gbm_dev);
        fprintf(stderr, "[Gralloc_Init] Opened GBM device fd=%d\n", g_gralloc.devfd);
        g_gralloc.gbm = gbm_create_device(g_gralloc.devfd);
        if (!g_gralloc.gbm) {
            close(g_gralloc.devfd);
            g_gralloc.devfd = -1;
            fprintf(stderr, "[Gralloc_Init] ERROR: gbm_create_device failed\n");
            die_msg("gbm_create_device failed");
        }
        fprintf(stderr, "[Gralloc_Init] GBM device initialized successfully\n");
        return;
    }

    fprintf(stderr, "[Gralloc_Init] Using DUMB backend with device: %s\n", dumb_dev);
    g_gralloc.backend = GRALLOC_BACKEND_DUMB;
    g_gralloc.devfd = open_dev(dumb_dev);
    fprintf(stderr, "[Gralloc_Init] Opened DUMB device fd=%d\n", g_gralloc.devfd);
    fprintf(stderr, "[Gralloc_Init] DUMB backend initialized\n");
}

void gralloc_deinit(void)
{
    fprintf(stderr, "[Gralloc_Deinit] Cleaning up gralloc resources\n");
    
    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        fprintf(stderr, "[Gralloc_Deinit] Destroying GBM device\n");
        if (g_gralloc.gbm) {
            gbm_device_destroy(g_gralloc.gbm);
            g_gralloc.gbm = NULL;
        }
        break;

    case GRALLOC_BACKEND_DUMB:
    case GRALLOC_BACKEND_NONE:
    default:
        break;
    }

    if (g_gralloc.devfd >= 0) {
        close(g_gralloc.devfd);
        g_gralloc.devfd = -1;
    }

    g_gralloc.backend = GRALLOC_BACKEND_NONE;
}

struct gralloc_image gralloc_alloc_image(EGLDisplay dpy, int width, int height)
{
    fprintf(stderr, "[Gralloc_Alloc] gralloc_alloc_image called: %dx%d\n", width, height);
    
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "[Gralloc_Alloc] ERROR: invalid width/height\n");
        die_msg("gralloc_alloc_image: invalid width/height");
    }

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        fprintf(stderr, "[Gralloc_Alloc] Using GBM backend\n");
        return alloc_image_from_gbm(dpy, width, height);

    case GRALLOC_BACKEND_DUMB:
        fprintf(stderr, "[Gralloc_Alloc] Using DUMB backend\n");
        return alloc_image_from_dumb(dpy, width, height);

    default:
        fprintf(stderr, "[Gralloc_Alloc] ERROR: invalid backend (%d)\n", g_gralloc.backend);
        die_msg("gralloc_alloc_image: invalid backend");
        return gralloc_image {};
    }
}

void gralloc_free_image(EGLDisplay dpy, struct gralloc_image img)
{
    fprintf(stderr, "[Gralloc_Free] Freeing image %p\n", img.image);
    
    if (img.image != EGL_NO_IMAGE_KHR) {
        if (!p_eglDestroyImageKHR(dpy, img.image)) {
            fprintf(stderr, "[Gralloc_Free] ERROR: eglDestroyImageKHR failed\n");
            die_msg("eglDestroyImageKHR failed");
        }
        fprintf(stderr, "[Gralloc_Free] EGLImage destroyed\n");
    }

    if (!img.priv) {
        fprintf(stderr, "[Gralloc_Free] No private data to free\n");
        return;
    }

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        fprintf(stderr, "[Gralloc_Free] Destroying GBM buffer object\n");
        gbm_bo_destroy((struct gbm_bo *)img.priv);
        return;

    case GRALLOC_BACKEND_DUMB: {
        uint32_t handle = (uint32_t)(uintptr_t)img.priv;
        fprintf(stderr, "[Gralloc_Free] Destroying DUMB buffer handle %u\n", handle);
        destroy_dumb_handle(handle);
        return;
    }

    default:
        fprintf(stderr, "[Gralloc_Free] ERROR: invalid backend (%d)\n", g_gralloc.backend);
        die_msg("gralloc_free_image: invalid backend");
        return;
    }
}
