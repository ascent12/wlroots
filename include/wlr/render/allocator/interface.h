#ifndef WLR_RENDER_ALLOCATOR_INTERFACE_H
#define WLR_RENDER_ALLOCATOR_INTERFACE_H

#include <stdint.h>

struct wlr_allocator;

struct wlr_allocator_impl {
	struct wlr_image *(*allocate)(struct wlr_allocator *alloc,
		uint32_t width, uint32_t height, uint32_t format,
		size_t num_modifiers, const uint64_t *modifiers);
	void (*deallocate)(struct wlr_allocator *alloc, struct wlr_image *img);
};

void wlr_allocator_init(struct wlr_allocator *alloc,
	const struct wlr_allocator_impl *impl);

#endif
