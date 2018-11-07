/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_H
#define WLR_BACKEND_H

// Needed for clockid_t
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 199309L
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 199309L
#endif

#include <stdbool.h>
#include <time.h>

#include <wayland-server.h>

struct wlr_backend_impl;
struct wlr_format_set;
struct wlr_gbm_image;
struct wlr_session;

struct wlr_backend {
	const struct wlr_backend_impl *impl;

	struct {
		/** Raised when destroyed, passed the wlr_backend reference */
		struct wl_signal destroy;
		/** Raised when new inputs are added, passed the wlr_input_device */
		struct wl_signal new_input;
		/** Raised when new outputs are added, passed the wlr_output */
		struct wl_signal new_output;
	} events;
};

/**
 * Automatically initializes the most suitable backend given the environment.
 * Will always return a multibackend. The backend is created but not started.
 * Returns NULL on failure.
 *
 * The compositor can request to initialize the backend's renderer by setting
 * the create_render_func. The callback must initialize the given wlr_egl and
 * return a valid wlr_renderer, or NULL if it has failed to initiaze it.
 * Pass NULL as create_renderer_func to use the backend's default renderer.
 */
struct wlr_backend *wlr_backend_autocreate(struct wl_display *display);

/**
 * Start the backend. This may signal new_input or new_output immediately, but
 * may also wait until the display's event loop begins. Returns false on
 * failure.
 */
bool wlr_backend_start(struct wlr_backend *backend);

/**
 * Destroy the backend and clean up all of its resources. Normally called
 * automatically when the wl_display is destroyed.
 */
void wlr_backend_destroy(struct wlr_backend *backend);

struct wlr_renderer;
struct wlr_renderer *wlr_backend_get_renderer(struct wlr_backend *backend);

struct wlr_format_set *wlr_backend_get_formats(struct wlr_backend *backend);

/**
 * Obtains the wlr_session reference from this backend if there is any.
 * Might return NULL for backends that don't use a session.
 */
struct wlr_session *wlr_backend_get_session(struct wlr_backend *backend);

/**
 * Returns the clock used by the backend for presentation feedback.
 */
clockid_t wlr_backend_get_presentation_clock(struct wlr_backend *backend);

/*
 * Returns the file descriptor of the render node used by this backend.
 * This file descriptor is suitable for using with gbm_create_device().
 * The backend retains ownership of this file descriptor, so it should
 * not be closed.
 *
 * Returns -1 if the backend does not have a render node.
 */
int wlr_backend_get_render_fd(struct wlr_backend *backend);

/*
 * Requests the backend to create local resources for 'img' so that it can be
 * used for presentation. Any such resources are expected to be added to
 * img->base.backend_priv.
 *
 * This function is used by wlr_gbm_allocator and generally shouldn't need to
 * be used elsewhere.
 *
 * Returns false if it failed or the backend does not support GBM.
 */
bool wlr_backend_attach_gbm(struct wlr_backend *backend, struct wlr_gbm_image *img);

/*
 * Requests the backend to destroy any local resources for 'img', found in
 * img->base.backend_priv.
 *
 * This function is used by wlr_gbm_allocator and generally shouldn't need to
 * be used elsewhere.
 */
void wlr_backend_detach_gbm(struct wlr_backend *backend, struct wlr_gbm_image *img);

#endif
