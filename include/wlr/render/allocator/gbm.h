#ifndef WLR_RENDER_ALLOCATOR_GBM_H
#define WLR_RENDER_ALLOCATOR_GBM_H

#include <stdint.h>
#include <gbm.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>

struct wlr_gbm_image {
	struct wlr_image base;

	struct gbm_bo *bo;
	void *renderer_priv;
};

typedef bool (*wlr_gbm_image_func_t)(void *, struct wlr_gbm_image *);

struct wlr_gbm_allocator {
	struct wlr_allocator base;

	struct wlr_backend *backend;
	struct gbm_device *gbm;

	void *userdata;
	wlr_gbm_image_func_t create;
	wlr_gbm_image_func_t destroy;
};

struct wlr_gbm_allocator *wlr_gbm_allocator_create(struct wlr_backend *backend,
	void *userdata, wlr_gbm_image_func_t create, wlr_gbm_image_func_t destroy);

void wlr_gbm_allocator_destroy(struct wlr_gbm_allocator *alloc);

#endif
