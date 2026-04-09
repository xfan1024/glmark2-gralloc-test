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
#ifdef GBM_BO_USE_WRITE
    if (strcmp(token, "write") == 0) {
        return GBM_BO_USE_WRITE;
    }
#endif
#ifdef GBM_BO_USE_LINEAR
    if (strcmp(token, "linear") == 0) {
        return GBM_BO_USE_LINEAR;
    }
#endif
#ifdef GBM_BO_USE_TEXTURING
    if (strcmp(token, "texturing") == 0) {
        return GBM_BO_USE_TEXTURING;
    }
#endif

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

    bo = gbm_bo_create(g_gralloc.gbm,
                       (uint32_t)width,
                       (uint32_t)height,
                       GBM_FORMAT_XRGB8888,
                       gbm_usage_from_env());
    if (!bo) {
        die_msg("gbm_bo_create failed");
    }

    dma_fd = gbm_bo_get_fd(bo);
    if (dma_fd < 0) {
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_fd failed");
    }

    stride = gbm_bo_get_stride(bo);
    if (stride == 0) {
        close(dma_fd);
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_stride returned 0");
    }

    EGLint attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };

    out.image = p_eglCreateImageKHR(dpy,
                                    EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT,
                                    NULL,
                                    attrs);

    close(dma_fd);

    if (out.image == EGL_NO_IMAGE_KHR) {
        gbm_bo_destroy(bo);
        die_msg("eglCreateImageKHR failed for GBM path");
    }

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

    EGLint attrs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)create_req.pitch,
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

    g_gralloc.backend = GRALLOC_BACKEND_NONE;
    g_gralloc.devfd = -1;
    g_gralloc.gbm = NULL;
    g_gralloc.get_proc_address = get_proc_address;

    init_egl_procs();

    if (gbm_dev && gbm_dev[0] && dumb_dev && dumb_dev[0]) {
        die_msg("GBM_DEV and DUMB_DEV are both set; exactly one must be set");
    }

    if ((!gbm_dev || !gbm_dev[0]) && (!dumb_dev || !dumb_dev[0])) {
        die_msg("Neither GBM_DEV nor DUMB_DEV is set; exactly one must be set");
    }

    if (gbm_dev && gbm_dev[0]) {
        g_gralloc.backend = GRALLOC_BACKEND_GBM;
        g_gralloc.devfd = open_dev(gbm_dev);
        g_gralloc.gbm = gbm_create_device(g_gralloc.devfd);
        if (!g_gralloc.gbm) {
            close(g_gralloc.devfd);
            g_gralloc.devfd = -1;
            die_msg("gbm_create_device failed");
        }
        return;
    }

    g_gralloc.backend = GRALLOC_BACKEND_DUMB;
    g_gralloc.devfd = open_dev(dumb_dev);
}

void gralloc_deinit(void)
{
    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
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
    if (width <= 0 || height <= 0) {
        die_msg("gralloc_alloc_image: invalid width/height");
    }

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        return alloc_image_from_gbm(dpy, width, height);

    case GRALLOC_BACKEND_DUMB:
        return alloc_image_from_dumb(dpy, width, height);

    default:
        die_msg("gralloc_alloc_image: invalid backend");
        return gralloc_image {};
    }
}

void gralloc_free_image(EGLDisplay dpy, struct gralloc_image img)
{
    if (img.image != EGL_NO_IMAGE_KHR) {
        if (!p_eglDestroyImageKHR(dpy, img.image)) {
            die_msg("eglDestroyImageKHR failed");
        }
    }

    if (!img.priv) {
        return;
    }

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        gbm_bo_destroy((struct gbm_bo *)img.priv);
        return;

    case GRALLOC_BACKEND_DUMB: {
        uint32_t handle = (uint32_t)(uintptr_t)img.priv;
        destroy_dumb_handle(handle);
        return;
    }

    default:
        die_msg("gralloc_free_image: invalid backend");
        return;
    }
}
