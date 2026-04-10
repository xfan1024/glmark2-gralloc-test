#ifndef __gralloc_eglimage__
#define __gralloc_eglimage__

#include <stdint.h>
#include <glad/egl.h>

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

/**
 * Begin GPU fence-based buffer resolution.
 * 
 * Call immediately after rendering. Creates an EGL fence to track GPU operations.
 * The fence is stored internally and will be waited for in gralloc_resolve_end().
 * 
 * @param img: the gralloc image to resolve
 * @param dpy: EGL display
 */
void gralloc_image_resolve_begin(struct gralloc_image img, EGLDisplay dpy);

/**
 * Complete buffer resolution by waiting for GPU fence.
 * 
 * Waits for the GPU fence created by gralloc_resolve_begin(), then:
 * 1. Performs DMA-BUF CPU sync start
 * 2. Accesses buffer pages to ensure coherency
 * 3. Performs DMA-BUF CPU sync end
 * 
 * Fallback to glFinish if fence creation failed in begin().
 * 
 * @param img: the gralloc image to resolve
 * @param dpy: EGL display
 */
void gralloc_image_resolve_end(struct gralloc_image img, EGLDisplay dpy);

/**
 * Convenience function: resolve DMA-BUF after GPU operations.
 * 
 * Uses glFinish directly for simpler semantics when start/end split is not needed.
 * Does NOT use fence mechanism - simpler but potentially less optimal for pipelining.
 * 
 * @param img: the gralloc image to resolve
 */
void gralloc_image_resolve(struct gralloc_image img);

#endif
