#ifndef WLR_RENDER_WLR_SWAPCHAIN_H
#define WLR_RENDER_WLR_SWAPCHAIN_H

#include <stdint.h>
#include <stdbool.h>

#include <gbm.h>

#include <wlr/render/wlr_renderer.h>

enum wlr_swapchain_flags {
	WLR_SWAPCHAIN_TRIPLE_BUFFERED = 1,
	WLR_SWAPCHAIN_USE_SCANOUT = 2,
};

struct wlr_image {
	struct wlr_swapchain *swapchain;
	struct gbm_bo *bo;
	void *renderer_priv;

	uint64_t seq;
	bool aquired;
};

struct wlr_swapchain {
	struct wlr_renderer *renderer;
	struct gbm_device *gbm;
	uint32_t flags;

	struct wlr_image images[3];

	uint64_t seq;
};

struct wlr_swapchain *wlr_swapchain_create(struct wlr_renderer *renderer,
		uint32_t width, uint32_t height, uint32_t format,
		const uint64_t *modifiers, size_t num_modifiers,
		uint32_t flags);

void wlr_swapchain_destroy(struct wlr_swapchain *sc);

struct wlr_image *wlr_swapchain_aquire(struct wlr_swapchain *sc);

void wlr_swapchain_release(struct wlr_swapchain *sc, struct wlr_image *image);

#endif
