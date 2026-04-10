#include "gralloc_egl_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#include <gbm.h>

#include "libmatrix/log.h"

/* Alias for KHR naming convention (glad provides EGL_NO_IMAGE) */
#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR EGL_NO_IMAGE
#endif

/* EGL Sync extension constants (EGL_KHR_fence_sync) */
#ifndef EGL_NO_SYNC_KHR
#define EGL_NO_SYNC_KHR EGL_NO_SYNC
#endif
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR EGL_SYNC_FENCE
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR EGL_FOREVER
#endif

/* EGL DMA-BUF extension constants (EGL_EXT_image_dma_buf_import) */
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT                0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT             0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT            0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT        0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT         0x3274
#endif

/* EGL function pointer types (avoid direct EGL header inclusion) */
typedef EGLImage (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, void *ctx, unsigned int target, void *buffer, const int *attrib_list);
typedef int (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImage image);
typedef EGLSync (*PFNEGLCREATESYNCKHRPROC)(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
typedef EGLint (*PFNEGLCLIENTWAITSYNCKHRPROC)(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTimeKHR timeout);
typedef EGLBoolean (*PFNEGLDESTROYSYNCKHPROC)(EGLDisplay dpy, EGLSync sync);

/* GL function pointer type */
typedef void (*PFNGLFINISHPROC)(void);

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
    size_t page_size;
};

struct gralloc_image_priv_base {
    int dma_fd;
    void *buf_ptr;
    size_t buf_size;
    EGLSync resolve_sync;  /* Fence for GPU sync */
};

struct gralloc_image_priv_gbm {
    struct gralloc_image_priv_base base;
    struct gbm_bo *bo;
};

struct gralloc_image_priv_dumb {
    struct gralloc_image_priv_base base;
    uint32_t handle;
};

static struct gralloc_platform g_gralloc = {};

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = NULL;
static PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR = NULL;
static PFNEGLCLIENTWAITSYNCKHRPROC p_eglClientWaitSyncKHR = NULL;
static PFNEGLDESTROYSYNCKHPROC p_eglDestroySyncKHR = NULL;
static PFNGLFINISHPROC p_glFinish = NULL;

static void die_errno(const char *msg)
{
    Log::error("%s: %s", msg, strerror(errno));
    exit(1);
}

static void die_msg(const char *msg)
{
    Log::error("%s", msg);
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

    p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)g_gralloc.get_proc_address("eglCreateSyncKHR");
    p_eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)g_gralloc.get_proc_address("eglClientWaitSyncKHR");
    p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHPROC)g_gralloc.get_proc_address("eglDestroySyncKHR");

    p_glFinish = (PFNGLFINISHPROC)g_gralloc.get_proc_address("glFinish");
    if (!p_glFinish) {
        die_msg("get_proc_address(\"glFinish\") failed");
    }

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

    Log::error("unknown GBM_USAGE token: %s", token);
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

static void cleanup_base(struct gralloc_image_priv_base *base)
{
    if (!base) {
        return;
    }

    /* Destroy pending sync if any */
    if (base->resolve_sync != EGL_NO_SYNC_KHR) {
        Log::warning("Destroying pending resolve_sync");
        p_eglDestroySyncKHR(eglGetDisplay(EGL_DEFAULT_DISPLAY), base->resolve_sync);
        base->resolve_sync = EGL_NO_SYNC_KHR;
    }

    /* Unmap buffer if it was mapped */
    if (base->buf_ptr != NULL && base->buf_size > 0) {
        if (munmap(base->buf_ptr, base->buf_size) < 0) {
            Log::warning("munmap failed: %s", strerror(errno));
        } else {
            if (g_gralloc.backend == GRALLOC_BACKEND_GBM) {
                Log::debug("[Gralloc_GBM] Buffer unmapped from DMA-BUF FD");
            } else if (g_gralloc.backend == GRALLOC_BACKEND_DUMB) {
                Log::debug("[Gralloc_DUMB] Buffer unmapped from device FD");
            } else {
                Log::debug("Buffer unmapped");
            }
        }
    }
    
    /* Close DMA-BUF FD (used for EGLImage, needed for both backends) */
    if (base->dma_fd >= 0) {
        close(base->dma_fd);
        if (g_gralloc.backend == GRALLOC_BACKEND_GBM) {
            Log::debug("[Gralloc_GBM] DMA-BUF FD closed");
        } else if (g_gralloc.backend == GRALLOC_BACKEND_DUMB) {
            Log::debug("[Gralloc_DUMB] DMA-BUF FD closed");
        } else {
            Log::debug("DMA-BUF FD closed");
        }
    }
}

/* Get base pointer from image (base is at offset 0 for both GBM and DUMB) */
static struct gralloc_image_priv_base *get_base_from_image(struct gralloc_image img)
{
    if (img.image == EGL_NO_IMAGE_KHR || !img.priv) {
        return NULL;
    }
    return (struct gralloc_image_priv_base *)img.priv;
}

/* Memory load helper: touch each page */
static void access_buffer_pages(struct gralloc_image_priv_base *base)
{
    if (!base || !base->buf_ptr || base->buf_size == 0) {
        return;
    }

    size_t num_pages = (base->buf_size + g_gralloc.page_size - 1) / g_gralloc.page_size;
    Log::debug("Accessing %zu pages (%zu bytes)", num_pages, base->buf_size);

    for (size_t i = 0; i < num_pages; i++) {
        volatile int *page_ptr = (volatile int *)((uintptr_t)base->buf_ptr + (i * g_gralloc.page_size));
        int dummy = *page_ptr;  /* Read first int from page */
        (void)dummy;  /* Prevent compiler optimization */
    }
}

/* DMA-BUF sync begin */
static void dma_buf_sync_begin(int dma_fd)
{
    struct dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
    if (ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        Log::warning("DMA_BUF_IOCTL_SYNC START failed: %s", strerror(errno));
    }
}

/* DMA-BUF sync end */
static void dma_buf_sync_end(int dma_fd)
{
    struct dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
    if (ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        Log::warning("DMA_BUF_IOCTL_SYNC END failed: %s", strerror(errno));
    }
}

static struct gralloc_image alloc_image_from_gbm(EGLDisplay dpy, int width, int height)
{
    struct gralloc_image out = {};
    struct gbm_bo *bo = NULL;
    int dma_fd = -1;
    uint32_t stride = 0;

    Log::debug("Allocating GBM buffer %dx%d", width, height);
    
    bo = gbm_bo_create(g_gralloc.gbm,
                       (uint32_t)width,
                       (uint32_t)height,
                       GBM_FORMAT_XRGB8888,
                       gbm_usage_from_env());
    if (!bo) {
        Log::error("gbm_bo_create failed");
        die_msg("gbm_bo_create failed");
    }
    Log::debug("GBM buffer object created");

    dma_fd = gbm_bo_get_fd(bo);
    if (dma_fd < 0) {
        Log::error("gbm_bo_get_fd failed");
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_fd failed");
    }
    Log::debug("DMA-BUF FD: %d", dma_fd);

    stride = gbm_bo_get_stride(bo);
    if (stride == 0) {
        Log::error("gbm_bo_get_stride returned 0");
        close(dma_fd);
        gbm_bo_destroy(bo);
        die_msg("gbm_bo_get_stride returned 0");
    }
    Log::debug("Buffer stride: %u bytes", stride);
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

    out.image = p_eglCreateImageKHR(dpy,
                                    EGL_NO_CONTEXT,
                                    EGL_LINUX_DMA_BUF_EXT,
                                    NULL,
                                    attrs);

    if (out.image == EGL_NO_IMAGE_KHR) {
        Log::error("eglCreateImageKHR failed");
        close(dma_fd);
        gbm_bo_destroy(bo);
        die_msg("eglCreateImageKHR failed for GBM path");
    }
    Log::debug("EGLImage created: %p", out.image);

    struct gralloc_image_priv_gbm *priv = (struct gralloc_image_priv_gbm *)malloc(sizeof(*priv));
    if (!priv) {
        p_eglDestroyImageKHR(dpy, out.image);
        close(dma_fd);
        gbm_bo_destroy(bo);
        die_msg("malloc failed");
    }
    
    size_t buf_size = (size_t)stride * (size_t)height;
    void *buf_ptr = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
    if (buf_ptr == MAP_FAILED) {
        Log::debug("[Gralloc_GBM] WARNING: mmap failed: %s (continuing without cpu access)", strerror(errno));
        buf_ptr = NULL;
    } else {
        Log::debug("[Gralloc_GBM] Buffer mapped: %p (size %zu)", buf_ptr, buf_size);
    }
    
    priv->base.dma_fd = dma_fd;  /* Keep open for lifetime of buffer */
    priv->base.buf_ptr = buf_ptr;
    priv->base.buf_size = buf_size;
    priv->base.resolve_sync = EGL_NO_SYNC_KHR;
    priv->bo = bo;
    
    out.priv = (void *)priv;
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

    if (out.image == EGL_NO_IMAGE_KHR) {
        close(dma_fd);
        destroy_dumb_handle(create_req.handle);
        die_msg("eglCreateImageKHR failed for dumb path");
    }

    struct gralloc_image_priv_dumb *priv = (struct gralloc_image_priv_dumb *)malloc(sizeof(*priv));
    if (!priv) {
        p_eglDestroyImageKHR(dpy, out.image);
        close(dma_fd);
        destroy_dumb_handle(create_req.handle);
        die_msg("malloc failed");
    }
    
    /* Use DRM_IOCTL_MODE_MAP_DUMB to get CPU-mappable offset */
    struct drm_mode_map_dumb map_req = {};
    map_req.handle = create_req.handle;
    if (drmIoctl(g_gralloc.devfd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
        Log::warning("DRM_IOCTL_MODE_MAP_DUMB failed: %s (continuing without CPU access)", strerror(errno));
        priv->base.dma_fd = dma_fd;
        priv->base.buf_ptr = NULL;
        priv->base.buf_size = 0;
        priv->base.resolve_sync = EGL_NO_SYNC_KHR;
        priv->handle = create_req.handle;
        out.priv = (void *)priv;
        return out;
    }
    
    /* mmap using the device fd and the offset from MAP_DUMB ioctl */
    size_t buf_size = (size_t)create_req.pitch * (size_t)height;
    void *buf_ptr = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, 
                         g_gralloc.devfd, map_req.offset);
    if (buf_ptr == MAP_FAILED) {
        Log::warning("mmap failed: %s (continuing without CPU access)", strerror(errno));
        buf_ptr = NULL;
    } else {
        Log::debug("[Gralloc_DUMB] Buffer mapped: %p (size %zu, offset 0x%llx)", 
                   buf_ptr, buf_size, (unsigned long long)map_req.offset);
    }
    
    priv->base.dma_fd = dma_fd;  /* Keep open for lifetime of buffer */
    priv->base.buf_ptr = buf_ptr;
    priv->base.buf_size = buf_size;
    priv->base.resolve_sync = EGL_NO_SYNC_KHR;
    priv->handle = create_req.handle;
    
    out.priv = (void *)priv;
    return out;
}

void gralloc_init(GRALLOC_EGLGETPROCADDRESS get_proc_address)
{
    const char *gbm_dev = getenv("GBM_DEV");
    const char *dumb_dev = getenv("DUMB_DEV");

    Log::info("Initializing gralloc");
    Log::debug("GBM_DEV=%s, DUMB_DEV=%s", 
            gbm_dev ? gbm_dev : "(not set)", 
            dumb_dev ? dumb_dev : "(not set)");

    g_gralloc.backend = GRALLOC_BACKEND_NONE;
    g_gralloc.devfd = -1;
    g_gralloc.gbm = NULL;
    g_gralloc.get_proc_address = get_proc_address;
    g_gralloc.page_size = sysconf(_SC_PAGESIZE);
    Log::debug("Page size: %zu bytes", g_gralloc.page_size);

    init_egl_procs();
    Log::debug("EGL procs initialized");

    if (gbm_dev && gbm_dev[0] && dumb_dev && dumb_dev[0]) {
        die_msg("GBM_DEV and DUMB_DEV are both set; exactly one must be set");
    }

    if ((!gbm_dev || !gbm_dev[0]) && (!dumb_dev || !dumb_dev[0])) {
        die_msg("Neither GBM_DEV nor DUMB_DEV is set; exactly one must be set");
    }

    if (gbm_dev && gbm_dev[0]) {
        Log::info("Using GBM backend: %s", gbm_dev);
        g_gralloc.backend = GRALLOC_BACKEND_GBM;
        g_gralloc.devfd = open_dev(gbm_dev);
        g_gralloc.gbm = gbm_create_device(g_gralloc.devfd);
        if (!g_gralloc.gbm) {
            close(g_gralloc.devfd);
            g_gralloc.devfd = -1;
            Log::error("gbm_create_device failed");
            die_msg("gbm_create_device failed");
        }
        Log::debug("GBM device ready");
        return;
    }

    Log::info("Using DUMB backend: %s", dumb_dev);
    g_gralloc.backend = GRALLOC_BACKEND_DUMB;
    g_gralloc.devfd = open_dev(dumb_dev);
    Log::debug("DUMB device ready");
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
        Log::error("invalid width/height: %dx%d", width, height);
        die_msg("gralloc_alloc_image: invalid width/height");
    }

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM:
        return alloc_image_from_gbm(dpy, width, height);

    case GRALLOC_BACKEND_DUMB:
        return alloc_image_from_dumb(dpy, width, height);

    default:
        Log::error("invalid backend: %d", g_gralloc.backend);
        die_msg("gralloc_alloc_image: invalid backend");
        return gralloc_image {};
    }
}

void gralloc_free_image(EGLDisplay dpy, struct gralloc_image img)
{
    if (img.image != EGL_NO_IMAGE_KHR) {
        if (!p_eglDestroyImageKHR(dpy, img.image)) {
            Log::error("eglDestroyImageKHR failed");
            die_msg("eglDestroyImageKHR failed");
        }
    }

    if (!img.priv) {
        return;
    }

    /* Clean up base (common for both backends) */
    struct gralloc_image_priv_base *base = (struct gralloc_image_priv_base *)img.priv;
    cleanup_base(base);

    switch (g_gralloc.backend) {
    case GRALLOC_BACKEND_GBM: {
        struct gralloc_image_priv_gbm *priv = (struct gralloc_image_priv_gbm *)img.priv;
        
        /* Destroy GBM buffer object */
        if (priv->bo) {
            gbm_bo_destroy(priv->bo);
        }
        
        free(priv);
        return;
    }

    case GRALLOC_BACKEND_DUMB: {
        struct gralloc_image_priv_dumb *priv = (struct gralloc_image_priv_dumb *)img.priv;
        
        /* Destroy DUMB buffer handle */
        destroy_dumb_handle(priv->handle);
        
        free(priv);
        return;
    }

    default:
        Log::error("invalid backend: %d", g_gralloc.backend);
        die_msg("gralloc_free_image: invalid backend");
        return;
    }
}

void gralloc_image_resolve_begin(struct gralloc_image img, EGLDisplay dpy)
{
    struct gralloc_image_priv_base *base = get_base_from_image(img);
    
    if (!base) {
        Log::error("invalid image");
        return;
    }

    base->resolve_sync = p_eglCreateSyncKHR(dpy, EGL_SYNC_FENCE_KHR, NULL);
    
    if (base->resolve_sync == EGL_NO_SYNC_KHR) {
        Log::warning("eglCreateSyncKHR failed, will use glFinish fallback");
    } else {
        Log::debug("Fence created");
    }
}

void gralloc_image_resolve_end(struct gralloc_image img, EGLDisplay dpy)
{
    struct gralloc_image_priv_base *base = get_base_from_image(img);
    
    if (!base) {
        Log::error("invalid image");
        return;
    }

    /* Wait for GPU fence if available */
    if (base->resolve_sync != EGL_NO_SYNC_KHR) {
        p_eglClientWaitSyncKHR(dpy, base->resolve_sync, 0, EGL_FOREVER_KHR);
        p_eglDestroySyncKHR(dpy, base->resolve_sync);
        base->resolve_sync = EGL_NO_SYNC_KHR;
    } else {
        Log::debug("No fence available, using glFinish fallback");
        p_glFinish();
    }

    /* Now safe to access buffer from CPU */
    if (base->dma_fd >= 0) {
        dma_buf_sync_begin(base->dma_fd);
        access_buffer_pages(base);
        dma_buf_sync_end(base->dma_fd);
    }
}

void gralloc_image_resolve(struct gralloc_image img)
{
    struct gralloc_image_priv_base *base = get_base_from_image(img);
    
    if (!base) {
        Log::error("invalid image");
        return;
    }

    /* Direct path using glFinish - simpler semantics */
    p_glFinish();

    /* Now safe to access buffer from CPU */
    if (base->dma_fd >= 0) {
        dma_buf_sync_begin(base->dma_fd);
        access_buffer_pages(base);
        dma_buf_sync_end(base->dma_fd);
    }
}
