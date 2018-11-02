#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <gbm.h>

#include <wlr/util/log.h>
#include "wlr/render/wlr_swapchain.h"

static struct gbm_bo *bo_create(struct gbm_device *gbm,
		uint32_t width, uint32_t height, uint32_t format,
		const uint64_t *modifiers, size_t num_modifiers, bool scanout) {
	if (modifiers) {
		return gbm_bo_create_with_modifiers(gbm, width, height, format, 
			modifiers, num_modifiers);
	} else {
		return gbm_bo_create(gbm, width, height, format, GBM_BO_USE_RENDERING);
	}
}

struct wlr_swapchain *wlr_swapchain_create(struct wlr_renderer *renderer,
		uint32_t width, uint32_t height, uint32_t format,
		const uint64_t *modifiers, size_t num_modifiers,
		uint32_t flags) {
	struct wlr_swapchain *sc = calloc(1, sizeof(*sc));
	if (!sc) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	sc->renderer = renderer;
	sc->flags = flags;

	int i;
	int len = flags & WLR_SWAPCHAIN_TRIPLE_BUFFERED ? 3 : 2;
	for (i = 0; i < len; ++i) {
		sc->images[i].swapchain = sc;
		sc->images[i].bo = bo_create(renderer->gbm, width, height, format,
			modifiers, num_modifiers, flags & WLR_SWAPCHAIN_USE_SCANOUT);
		if (!sc->images[i].bo) {
			wlr_log_errno(WLR_ERROR, "Failed to create buffer");
			goto error_images;
		}
	}

	return sc;

error_images:
	for (int j = 0; j < i; ++j) {
		gbm_bo_destroy(sc->images[i].bo);
	}
	free(sc);

	return NULL;
}

void wlr_swapchain_destroy(struct wlr_swapchain *sc) {
	if (!sc) {
		return;
	}

	int len = sc->flags & WLR_SWAPCHAIN_TRIPLE_BUFFERED ? 3 : 2;
	for (int i = 0; i < len; ++i) {
		assert(!sc->images[i].aquired);
		gbm_bo_destroy(sc->images[i].bo);
	}
	free(sc);
}

struct wlr_image *wlr_swapchain_aquire(struct wlr_swapchain *sc) {
	struct wlr_image *ret = NULL;

	int len = sc->flags & WLR_SWAPCHAIN_TRIPLE_BUFFERED ? 3 : 2;
	for (int i = 0; i < len; ++i) {
		if (sc->images[i].aquired) {
			continue;
		}

		if (!ret || sc->images[i].seq < ret->seq) {
			ret = &sc->images[i];
		}
	}

	if (ret) {
		ret->aquired = true;
	}
	return ret;
}

void wlr_swapchain_release(struct wlr_swapchain *sc, struct wlr_image *image) {
	assert(image->swapchain == sc);
	assert(image->aquired);

	image->aquired = false;
	image->seq = ++sc->seq;
}
