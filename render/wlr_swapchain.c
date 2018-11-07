#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#include <gbm.h>

#include <wlr/util/log.h>
#include "wlr/render/wlr_swapchain.h"

struct wlr_swapchain *wlr_swapchain_create(struct wlr_allocator *alloc,
		uint32_t width, uint32_t height, uint32_t format,
		size_t num_modifiers, const uint64_t *modifiers,
		uint32_t flags) {
	struct wlr_swapchain *sc = calloc(1, sizeof(*sc));
	if (!sc) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	sc->alloc = alloc;
	sc->flags = flags;
	sc->num_images = flags & WLR_SWAPCHAIN_TRIPLE_BUFFERED ? 3 : 2;

	size_t i;
	for (i = 0; i < sc->num_images; ++i) {
		sc->images[i].img = wlr_allocator_allocate(alloc,
			width, height, format, num_modifiers, modifiers);
		if (!sc->images[i].img) {
			wlr_log_errno(WLR_ERROR, "Failed to create image");
			goto error_images;
		}
	}

	return sc;

error_images:
	for (size_t j = 0; j < i; ++j) {
		wlr_allocator_deallocate(alloc, sc->images[i].img);
	}
	free(sc);

	return NULL;
}

void wlr_swapchain_destroy(struct wlr_swapchain *sc) {
	if (!sc) {
		return;
	}

	for (size_t i = 0; i < sc->num_images; ++i) {
		assert(!sc->images[i].aquired);
		wlr_allocator_deallocate(sc->alloc, sc->images[i].img);
	}
	free(sc);
}

struct wlr_image *wlr_swapchain_aquire(struct wlr_swapchain *sc) {
	ssize_t i = -1;

	for (ssize_t j = 0; j < (ssize_t)sc->num_images; ++j) {
		if (sc->images[j].aquired) {
			continue;
		}

		if (i < 0 || sc->images[j].seq < sc->images[i].seq) {
			i = j;
		}
	}

	if (i >= 0) {
		sc->images[i].aquired = true;
		return sc->images[i].img;
	}
	return NULL;
}

void wlr_swapchain_release(struct wlr_swapchain *sc, struct wlr_image *image) {
	size_t i;
	for (i = 0; i < sc->num_images; ++i) {
		if (sc->images[i].img == image) {
			break;
		}
	}

	assert(i < sc->num_images);
	assert(sc->images[i].aquired);

	sc->images[i].aquired = false;
	sc->images[i].seq = ++sc->seq;
}
