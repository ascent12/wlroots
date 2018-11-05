/*
 * Hack just to let the linker work until I fix the backends
 */

#define _POSIX_C_SOURCE 199309L
#include <time.h>

struct wlr_renderer;
struct wlr_backend;
struct wlr_backend_impl;

struct wlr_renderer *wlr_backend_get_renderer(struct wlr_backend *b) {
	return NULL;
}
