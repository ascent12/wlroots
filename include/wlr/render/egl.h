/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_EGL_H
#define WLR_RENDER_EGL_H

#include <stdbool.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pixman.h>
#include <wayland-server.h>
#include <wlr/render/dmabuf.h>

struct wlr_egl {
	EGLDisplay display;
	EGLContext context;

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;

	PFNEGLBINDWAYLANDDISPLAYWL bind_wl_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_wl_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_wl_buffer;

	bool has_modifiers;
	bool has_bind_wl;

	struct wl_display *wl_display;
};

/**
 * Initializes an EGL context for the given platform and remote display.
 * Will attempt to load all possibly required api functions.
 */
struct wlr_egl *wlr_egl_create(struct gbm_device *gbm);

/**
 * Frees all related EGL resources, makes the context not-current and
 * unbinds a bound wayland display.
 */
void wlr_egl_destroy(struct wlr_egl *egl);

/**
 * Binds the given display to the EGL instance.
 * This will allow clients to create EGL surfaces from wayland ones and render
 * to it.
 */
bool wlr_egl_bind_display(struct wlr_egl *egl, struct wl_display *local_display);

/**
 * Creates an EGL image from the given wl_drm buffer resource.
 */
EGLImageKHR wlr_egl_create_image_from_wl_drm(struct wlr_egl *egl,
	struct wl_resource *data, EGLint *fmt, int *width, int *height,
	bool *inverted_y);

/**
 * Creates an EGL image from the given dmabuf attributes. Check usability
 * of the dmabuf with wlr_egl_check_import_dmabuf once first.
 */
EGLImageKHR wlr_egl_create_image_from_dmabuf(struct wlr_egl *egl,
	struct wlr_dmabuf_attributes *attributes);

#if 0
/**
 * Get the available dmabuf formats
 */
int wlr_egl_get_dmabuf_formats(struct wlr_egl *egl, int **formats);

/**
 * Get the available dmabuf modifiers for a given format
 */
int wlr_egl_get_dmabuf_modifiers(struct wlr_egl *egl, int format,
	uint64_t **modifiers);

bool wlr_egl_export_image_to_dmabuf(struct wlr_egl *egl, EGLImageKHR image,
	int32_t width, int32_t height, uint32_t flags,
	struct wlr_dmabuf_attributes *attribs);
#endif

/**
 * Destroys an EGL image created with the given wlr_egl.
 */
bool wlr_egl_destroy_image(struct wlr_egl *egl, EGLImageKHR image);

bool wlr_egl_make_current(struct wlr_egl *egl);

bool wlr_egl_is_current(struct wlr_egl *egl);

#endif
