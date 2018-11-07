#ifndef WLR_RENDER_ALLOCATOR_H
#define WLR_RENDER_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

struct wlr_allocator_impl;

struct wlr_allocator {
	const struct wlr_allocator_impl *impl;
};

struct wlr_image {
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint64_t modifier;

	void *backend_priv;
};

struct wlr_image *wlr_allocator_allocate(struct wlr_allocator *alloc,
	uint32_t width, uint32_t height, uint32_t format,
	size_t num_modifiers, const uint64_t *modifiers);

void wlr_allocator_deallocate(struct wlr_allocator *alloc, struct wlr_image *img);

#endif
