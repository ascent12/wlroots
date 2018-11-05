/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_BACKEND_INTERFACE_H
#define WLR_BACKEND_INTERFACE_H

// Needed for clockid_t
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 199309L
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 199309L
#endif

#include <stdbool.h>
#include <time.h>

struct wlr_backend;
struct wlr_format_set;
struct wlr_session;

struct wlr_backend_impl {
	bool (*start)(struct wlr_backend *backend);
	void (*destroy)(struct wlr_backend *backend);
	int (*get_render_fd)(struct wlr_backend *backend);
	struct wlr_format_set *(*get_formats)(struct wlr_backend *backend);
	struct wlr_session *(*get_session)(struct wlr_backend *backend);
	clockid_t (*get_presentation_clock)(struct wlr_backend *backend);
};

/**
 * Initializes common state on a wlr_backend and sets the implementation to the
 * provided wlr_backend_impl reference.
 */
void wlr_backend_init(struct wlr_backend *backend,
		const struct wlr_backend_impl *impl);

#endif
