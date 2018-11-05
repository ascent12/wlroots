#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <gbm.h>
#include <wayland-client.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/log.h>

#include "backend/wayland.h"
#include "util/signal.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"

static struct wlr_wl_output *get_wl_output_from_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output_is_wl(wlr_output));
	return (struct wlr_wl_output *)wlr_output;
}

static struct wl_callback_listener frame_listener;

static void surface_frame_callback(void *data, struct wl_callback *cb,
		uint32_t time) {
	struct wlr_wl_output *output = data;

	wl_callback_destroy(output->frame_callback);

	if (output->scheduled) {
		struct wlr_wl_buffer *buffer = gbm_bo_get_user_data(output->scheduled);
		wl_surface_attach(output->surface, buffer->buffer, 0, 0);
		//wl_surface_damage_buffer(output->surface, 0, 0,
		//	INT32_MAX, INT32_MAX);
		wl_surface_commit(output->surface);

		output->frame_callback = wl_surface_frame(output->surface);
		wl_callback_add_listener(output->frame_callback, &frame_listener, output);

		output->scheduled = NULL;
		wlr_log(WLR_DEBUG, "Committed");
	} else {
		output->frame_callback = NULL;
	}

	wlr_output_send_frame(&output->wlr_output);
}

static struct wl_callback_listener frame_listener = {
	.done = surface_frame_callback
};

static void output_transform(struct wlr_output *wlr_output,
		enum wl_output_transform transform) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	output->wlr_output.transform = transform;
}

static bool output_set_cursor(struct wlr_output *wlr_output,
		struct wlr_texture *texture, int32_t scale,
		enum wl_output_transform transform,
		int32_t hotspot_x, int32_t hotspot_y, bool update_texture) {
#if 0
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	struct wlr_wl_backend *backend = output->backend;

	struct wlr_box hotspot = { .x = hotspot_x, .y = hotspot_y };
	wlr_box_transform(&hotspot,
		wlr_output_transform_invert(wlr_output->transform),
		output->cursor.width, output->cursor.height, &hotspot);

	// TODO: use output->wlr_output.transform to transform pixels and hotpot
	output->cursor.hotspot_x = hotspot.x;
	output->cursor.hotspot_y = hotspot.y;

	if (!update_texture) {
		// Update hotspot without changing cursor image
		update_wl_output_cursor(output);
		return true;
	}

	if (output->cursor.surface == NULL) {
		output->cursor.surface =
			wl_compositor_create_surface(backend->compositor);
	}
	struct wl_surface *surface = output->cursor.surface;

	if (texture != NULL) {
		int width, height;
		wlr_texture_get_size(texture, &width, &height);
		width = width * wlr_output->scale / scale;
		height = height * wlr_output->scale / scale;

		output->cursor.width = width;
		output->cursor.height = height;

		if (output->cursor.egl_window == NULL) {
			output->cursor.egl_window =
				wl_egl_window_create(surface, width, height);
		}
		wl_egl_window_resize(output->cursor.egl_window, width, height, 0, 0);

		EGLSurface egl_surface =
			wlr_egl_create_surface(&backend->egl, output->cursor.egl_window);

		wlr_egl_make_current(&backend->egl, egl_surface, NULL);

		struct wlr_box cursor_box = {
			.width = width,
			.height = height,
		};

		float projection[9];
		wlr_matrix_projection(projection, width, height, wlr_output->transform);

		float matrix[9];
		wlr_matrix_project_box(matrix, &cursor_box, transform, 0, projection);

		wlr_renderer_begin(backend->renderer, width, height);
		wlr_renderer_clear(backend->renderer, (float[]){ 0.0, 0.0, 0.0, 0.0 });
		wlr_render_texture_with_matrix(backend->renderer, texture, matrix, 1.0);
		wlr_renderer_end(backend->renderer);

		wlr_egl_swap_buffers(&backend->egl, egl_surface, NULL);
		wlr_egl_destroy_surface(&backend->egl, egl_surface);
	} else {
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
	}

	update_wl_output_cursor(output);
	return true;
#endif
	return false;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);
	if (output == NULL) {
		return;
	}

	wl_list_remove(&output->link);

	if (output->cursor.surface) {
		wl_surface_destroy(output->cursor.surface);
	}

	if (output->frame_callback) {
		wl_callback_destroy(output->frame_callback);
	}

	zxdg_toplevel_v6_destroy(output->xdg_toplevel);
	zxdg_surface_v6_destroy(output->xdg_surface);
	wl_surface_destroy(output->surface);
	free(output);
}

void update_wl_output_cursor(struct wlr_wl_output *output) {
	if (output->backend->pointer && output->enter_serial) {
		wl_pointer_set_cursor(output->backend->pointer, output->enter_serial,
			output->cursor.surface, output->cursor.hotspot_x,
			output->cursor.hotspot_y);
	}
}

static bool output_move_cursor(struct wlr_output *_output, int x, int y) {
	// TODO: only return true if x == current x and y == current y
	return true;
}

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	struct gbm_bo *bo = data;
	struct wlr_wl_buffer *buffer = gbm_bo_get_user_data(bo);
	struct wlr_wl_output *output = buffer->output;

	struct wlr_output_event_release_buffer event = {
		.bo = bo,
		.userdata = buffer->userdata,
	};

	wl_signal_emit(&output->wlr_output.events.release_buffer, &event);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

static void free_wl_buffer(struct gbm_bo *bo, void *data) {
	struct wlr_wl_buffer *buffer = data;

	wl_buffer_destroy(buffer->buffer);
	free(buffer);
}

static bool output_schedule_frame(struct wlr_output *wlr_output, struct gbm_bo *bo,
		void *userdata) {
	struct wlr_wl_output *output = get_wl_output_from_output(wlr_output);

	if (output->scheduled) {
		struct wlr_wl_buffer *buffer = gbm_bo_get_user_data(output->scheduled);
		struct wlr_output_event_release_buffer event = {
			.bo = output->scheduled,
			.userdata = buffer->userdata,
		};

		wl_signal_emit(&output->wlr_output.events.release_buffer, &event);
		output->scheduled = NULL;
	}

	struct wlr_wl_buffer *buffer = gbm_bo_get_user_data(bo);
	if (!buffer) {
		buffer = calloc(1, sizeof(*buffer));
		if (!buffer) {
			wlr_log_errno(WLR_ERROR, "Allocation failed");
			output->scheduled = bo;
			return false;
		}

		struct zwp_linux_buffer_params_v1 *params =
			zwp_linux_dmabuf_v1_create_params(output->backend->dmabuf);

		int fd = gbm_bo_get_fd(bo);
		uint64_t mod = gbm_bo_get_modifier(bo);

		int n = gbm_bo_get_plane_count(bo);
		for (int i = 0; i < n; ++i) {
			zwp_linux_buffer_params_v1_add(params, fd, i,
				gbm_bo_get_offset(bo, i),
				gbm_bo_get_stride_for_plane(bo, i),
				mod >> 32, mod & 0xffffffff);
		}

		buffer->output = output;
		buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
			gbm_bo_get_width(bo), gbm_bo_get_height(bo),
			gbm_bo_get_format(bo), 0);
		buffer->userdata = userdata;
		wl_buffer_add_listener(buffer->buffer, &buffer_listener, bo);

		zwp_linux_buffer_params_v1_destroy(params);

		gbm_bo_set_user_data(bo, buffer, free_wl_buffer);
	} else {
		buffer->userdata = userdata;
	}

	if (!output->frame_callback) {
		output->frame_callback = wl_surface_frame(output->surface);
		wl_callback_add_listener(output->frame_callback, &frame_listener,
			output);

		wl_surface_attach(output->surface, buffer->buffer, 0, 0);
		//wl_surface_damage_buffer(output->surface, 0, 0,
		//	INT32_MAX, INT32_MAX);
		wl_surface_commit(output->surface);

		wlr_log(WLR_DEBUG, "Committed");
	} else {
		output->scheduled = bo;
	}

	return true;
}

static const struct wlr_output_impl output_impl = {
	.transform = output_transform,
	.destroy = output_destroy,
	.set_cursor = output_set_cursor,
	.move_cursor = output_move_cursor,
	.schedule_frame = output_schedule_frame,
};

bool wlr_output_is_wl(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static void xdg_surface_handle_configure(void *data, struct zxdg_surface_v6 *xdg_surface,
		uint32_t serial) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_surface == xdg_surface);

	zxdg_surface_v6_ack_configure(xdg_surface, serial);

	// nothing else?
}

static struct zxdg_surface_v6_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data,
		struct zxdg_toplevel_v6 *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	if (width == 0 || height == 0) {
		return;
	}

	// loop over states for maximized etc?
	wlr_output_update_custom_mode(&output->wlr_output, width, height, 0);
}

static void xdg_toplevel_handle_close(void *data,
		struct zxdg_toplevel_v6 *xdg_toplevel) {
	struct wlr_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	wlr_output_destroy(&output->wlr_output);
}

static struct zxdg_toplevel_v6_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

struct wlr_output *wlr_wl_output_create(struct wlr_backend *wlr_backend) {
	struct wlr_wl_backend *backend = get_wl_backend_from_backend(wlr_backend);
	if (!backend->started) {
		++backend->requested_outputs;
		return NULL;
	}

	struct wlr_wl_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	wlr_output_init(&output->wlr_output, &backend->backend, &output_impl,
		backend->local_display);
	struct wlr_output *wlr_output = &output->wlr_output;

	wlr_output_update_custom_mode(wlr_output, 1280, 720, 0);
	strncpy(wlr_output->make, "wayland", sizeof(wlr_output->make));
	strncpy(wlr_output->model, "wayland", sizeof(wlr_output->model));
	snprintf(wlr_output->name, sizeof(wlr_output->name), "WL-%d",
		wl_list_length(&backend->outputs) + 1);

	output->backend = backend;

	output->surface = wl_compositor_create_surface(backend->compositor);
	if (!output->surface) {
		wlr_log_errno(WLR_ERROR, "Could not create output surface");
		goto error;
	}
	wl_surface_set_user_data(output->surface, output);
	output->xdg_surface =
		zxdg_shell_v6_get_xdg_surface(backend->shell, output->surface);
	if (!output->xdg_surface) {
		wlr_log_errno(WLR_ERROR, "Could not get xdg surface");
		goto error;
	}
	output->xdg_toplevel =
		zxdg_surface_v6_get_toplevel(output->xdg_surface);
	if (!output->xdg_toplevel) {
		wlr_log_errno(WLR_ERROR, "Could not get xdg toplevel");
		goto error;
	}

	char title[32];
	if (snprintf(title, sizeof(title), "wlroots - %s", wlr_output->name)) {
		zxdg_toplevel_v6_set_title(output->xdg_toplevel, title);
	}

	zxdg_toplevel_v6_set_app_id(output->xdg_toplevel, "wlroots");
	zxdg_surface_v6_add_listener(output->xdg_surface,
			&xdg_surface_listener, output);
	zxdg_toplevel_v6_add_listener(output->xdg_toplevel,
			&xdg_toplevel_listener, output);
	wl_surface_commit(output->surface);

	wl_display_roundtrip(output->backend->remote_display);

	//output->frame_callback = wl_surface_frame(output->surface);
	//wl_callback_add_listener(output->frame_callback, &frame_listener, output);

	wl_list_insert(&backend->outputs, &output->link);
	wlr_output_update_enabled(wlr_output, true);

	wlr_signal_emit_safe(&backend->backend.events.new_output, wlr_output);

	if (backend->pointer != NULL) {
		create_wl_pointer(backend->pointer, output);
	}

	return wlr_output;

error:
	wlr_output_destroy(&output->wlr_output);
	return NULL;
}
