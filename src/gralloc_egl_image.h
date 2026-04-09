#ifndef __gralloc_eglimage__
#define __gralloc_eglimage__

#include <stdint.h>
#include <glad/egl.h>

/* Alias for KHR naming convention (glad provides EGL_NO_IMAGE) */
#ifndef EGL_NO_IMAGE_KHR
#define EGL_NO_IMAGE_KHR EGL_NO_IMAGE
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

/* Function pointer type for EGL proc loader */
typedef void* (*GRALLOC_EGLGETPROCADDRESS)(const char *procname);

struct gralloc_image {
    EGLImage image;
    void *priv;
};

/**
 * Initialize gralloc module with a custom EGL proc loader.
 * 
 * @param get_proc_address: function pointer to eglGetProcAddress or equivalent
 */
void gralloc_init(GRALLOC_EGLGETPROCADDRESS get_proc_address);
void gralloc_deinit(void);

struct gralloc_image gralloc_alloc_image(EGLDisplay dpy, int width, int height);
void gralloc_free_image(EGLDisplay dpy, struct gralloc_image img);

#endif
