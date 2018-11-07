#ifndef WLR_RENDER_WLR_SWAPCHAIN_H
#define WLR_RENDER_WLR_SWAPCHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gbm.h>

#include <wlr/render/allocator.h>

enum wlr_swapchain_flags {
	WLR_SWAPCHAIN_TRIPLE_BUFFERED = 1,
	WLR_SWAPCHAIN_USE_SCANOUT = 2,
};

struct wlr_swapchain {
	struct wlr_allocator *alloc;
	uint32_t flags;

	size_t num_images;
	struct {
		struct wlr_image *img;
		uint64_t seq;
		bool aquired;
	} images[3];

	uint64_t seq;
};

struct wlr_swapchain *wlr_swapchain_create(struct wlr_allocator *alloc,
		uint32_t width, uint32_t height, uint32_t format,
		size_t num_modifiers, const uint64_t *modifiers,
		uint32_t flags);

void wlr_swapchain_destroy(struct wlr_swapchain *sc);

struct wlr_image *wlr_swapchain_aquire(struct wlr_swapchain *sc);

void wlr_swapchain_release(struct wlr_swapchain *sc, struct wlr_image *image);

#endif
