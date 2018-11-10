#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/config.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <wayland-server.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#ifdef WLR_HAS_XCB_XKB
#include <xcb/xkb.h>
#endif

#include <wlr/backend/interface.h>
#include <wlr/backend/x11.h>
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/allocator/gbm.h>
#include <wlr/util/log.h>
#include <wlr/util/wlr_format_set.h>

#include "backend/x11.h"
#include "util/signal.h"

#include <fcntl.h>

struct wlr_x11_output *output_from_window(struct wlr_x11_backend *x11,
		xcb_window_t window) {
	struct wlr_x11_output *output;
	wl_list_for_each(output, &x11->outputs, link) {
		if (output->win == window) {
			return output;
		}
	}
	return NULL;
}

#if 0
static void handle_x11_event(struct wlr_x11_backend *x11,
		xcb_generic_event_t *event) {
	handle_x11_input_event(x11, event);

	switch (event->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
	case XCB_EXPOSE: {
		xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
		struct wlr_x11_output *output =
			get_x11_output_from_window_id(x11, ev->window);
		if (output != NULL) {
			wlr_output_update_needs_swap(&output->wlr_output);
		}
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *ev = (xcb_client_message_event_t *)event;
		if (ev->data.data32[0] == x11->atoms.wm_delete_window) {
			struct wlr_x11_output *output =
				get_x11_output_from_window_id(x11, ev->window);
			if (output != NULL) {
				wlr_output_destroy(&output->wlr_output);
			}
		}
		break;
	}
	}
}
#endif

static void handle_ext_event(struct wlr_x11_backend *x11,
		xcb_ge_generic_event_t *e) {
	if (e->extension == x11->present_opcode) {
		handle_present_event(x11, (xcb_present_generic_event_t *)e);
	} else if (e->extension == x11->xinput_opcode) {
		handle_xinput_event(x11, e);
	}
}

static void handle_client_message(struct wlr_x11_backend *x11,
		xcb_client_message_event_t *e) {
	if (e->data.data32[0] == x11->atoms.wm_delete_window) {
		struct wlr_x11_output *output = output_from_window(x11, e->window);
		if (output) {
			wlr_output_destroy(&output->wlr_output);
		}
	}
}

static int x11_event(int fd, uint32_t mask, void *data) {
	struct wlr_x11_backend *x11 = data;

	if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
		wl_display_terminate(x11->wl_display);
		return 0;
	}

	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(x11->xcb))) {
		switch (e->response_type & XCB_EVENT_RESPONSE_TYPE_MASK) {
		case XCB_GE_GENERIC:
			handle_ext_event(x11, (xcb_ge_generic_event_t *)e);
			break;
		case XCB_CLIENT_MESSAGE:
			handle_client_message(x11, (xcb_client_message_event_t *)e);
			break;
		}
		free(e);
	}

	return 0;
}

struct wlr_x11_backend *get_x11_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_x11(wlr_backend));
	return (struct wlr_x11_backend *)wlr_backend;
}

static bool backend_start(struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	x11->started = true;

	wlr_signal_emit_safe(&x11->backend.events.new_input, &x11->keyboard_dev);

	for (size_t i = 0; i < x11->requested_outputs; ++i) {
		wlr_x11_output_create(&x11->backend);
	}

	return true;
}

static void backend_destroy(struct wlr_backend *backend) {
	if (!backend) {
		return;
	}

	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);

	struct wlr_x11_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &x11->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_input_device_destroy(&x11->keyboard_dev);

	wlr_signal_emit_safe(&backend->events.destroy, backend);

	wl_event_source_remove(x11->event_source);
	wl_list_remove(&x11->display_destroy.link);

	xcb_disconnect(x11->xcb);
	free(x11);
}

struct wlr_format_set *backend_get_formats(struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	return &x11->formats;
}

static int backend_get_render_fd(struct wlr_backend *backend) {
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	return x11->render_fd;
}

static bool backend_attach_gbm(struct wlr_backend *backend, struct wlr_gbm_image *img) {
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	xcb_pixmap_t pixmap = xcb_generate_id(x11->xcb);
	xcb_void_cookie_t cookie;

	if (x11->has_modifiers) {
		uint32_t width = gbm_bo_get_width(img->bo);
		uint32_t height = gbm_bo_get_height(img->bo);
		uint32_t bpp = gbm_bo_get_bpp(img->bo);
		uint64_t mod = gbm_bo_get_modifier(img->bo);
		int32_t fds[4] = { -1, -1, -1, -1 };
		uint32_t strides[4] = { 0 };
		uint32_t offsets[4] = { 0 };

		int fd = gbm_bo_get_fd(img->bo);
		int n = gbm_bo_get_plane_count(img->bo);
		for (int i = 0; i < n; ++i) {
			fds[i] = fd;
			strides[i] = gbm_bo_get_stride_for_plane(img->bo, i);
			offsets[i] = gbm_bo_get_offset(img->bo, i);
		}

		cookie = xcb_dri3_pixmap_from_buffers_checked(x11->xcb, pixmap,
			x11->root, n, width, height, strides[0], offsets[0],
			strides[1], offsets[1], strides[2], offsets[2],
			strides[3], offsets[3], 24, bpp, mod, fds);

	} else {
		if (gbm_bo_get_plane_count(img->bo) != 1) {
			wlr_log(WLR_ERROR, "DRI 1.0 only supports single plane formats");
			return false;
		}

		int32_t fd = gbm_bo_get_fd(img->bo);
		uint32_t width = gbm_bo_get_width(img->bo);
		uint32_t height = gbm_bo_get_height(img->bo);
		uint32_t stride = gbm_bo_get_stride(img->bo);
		uint32_t bpp = gbm_bo_get_bpp(img->bo);

		cookie = xcb_dri3_pixmap_from_buffer_checked(x11->xcb, pixmap,
			x11->root, height * stride, width, height, stride,
			24, bpp, fd);
	}

	xcb_generic_error_t *err = xcb_request_check(x11->xcb, cookie);
	if (err) {
		wlr_log(WLR_ERROR, "Failed to create X11 pixmap");
		free(err);
		return false;
	}

	img->base.backend_priv = (void *)(uintptr_t)pixmap;

	return true;
}

static void backend_detach_gbm(struct wlr_backend *backend, struct wlr_gbm_image *img) {
	struct wlr_x11_backend *x11 = get_x11_backend_from_backend(backend);
	xcb_pixmap_t pixmap = (xcb_pixmap_t)(uintptr_t)img->base.backend_priv;

	xcb_free_pixmap(x11->xcb, pixmap);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_formats = backend_get_formats,
	.get_render_fd = backend_get_render_fd,
	.attach_gbm = backend_attach_gbm,
	.detach_gbm = backend_detach_gbm,
};

bool wlr_backend_is_x11(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_x11_backend *x11 =
		wl_container_of(listener, x11, display_destroy);
	backend_destroy(&x11->backend);
}

struct wlr_backend *wlr_x11_backend_create(struct wl_display *display,
		const char *x11_display) {
	struct wlr_x11_backend *x11 = calloc(1, sizeof(*x11));
	if (!x11) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	x11->xcb = xcb_connect(x11_display, NULL);
	if (!x11->xcb) {
		wlr_log(WLR_ERROR, "Failed to open X connection");
		goto error_x11;
	}

	struct {
		const char *name;
		xcb_intern_atom_cookie_t cookie;
		xcb_atom_t *atom;
	} atom_info[] = {
		{ .name = "WM_PROTOCOLS", .atom = &x11->atoms.wm_protocols },
		{ .name = "WM_DELETE_WINDOW", .atom = &x11->atoms.wm_delete_window },
		{ .name = "_NET_WM_NAME", .atom = &x11->atoms.net_wm_name },
		{ .name = "UTF8_STRING", .atom = &x11->atoms.utf8_string },
	};

	/*
	 * Queue all of these requests to the X server now, and do some other
	 * stuff and get the replies later.
	 */

	for (size_t i = 0; i < sizeof(atom_info) / sizeof(atom_info[0]); ++i) {
		atom_info[i].cookie = xcb_intern_atom(x11->xcb,
			true, strlen(atom_info[i].name), atom_info[i].name);
	}
	xcb_prefetch_extension_data(x11->xcb, &xcb_dri3_id);
	xcb_prefetch_extension_data(x11->xcb, &xcb_present_id);
	xcb_prefetch_extension_data(x11->xcb, &xcb_xfixes_id);
	xcb_prefetch_extension_data(x11->xcb, &xcb_input_id);
#ifdef WLR_HAS_XCB_XKB
	xcb_prefetch_extension_data(x11->xcb, &xcb_xkb_id);
#endif
	xcb_flush(x11->xcb);

	const xcb_setup_t *setup = xcb_get_setup(x11->xcb);
	xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;

	wlr_backend_init(&x11->backend, &backend_impl);

	x11->wl_display = display;
	x11->root = screen->root;
	x11->visual = screen->root_visual;
	wl_list_init(&x11->outputs);

	wlr_input_device_init(&x11->keyboard_dev, WLR_INPUT_DEVICE_KEYBOARD,
		&input_device_impl, "X11 keyboard", 0, 0);
	wlr_keyboard_init(&x11->keyboard, &keyboard_impl);
	x11->keyboard_dev.keyboard = &x11->keyboard;

	int fd = xcb_get_file_descriptor(x11->xcb);
	struct wl_event_loop *ev = wl_display_get_event_loop(display);
	uint32_t events = WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;
	x11->event_source = wl_event_loop_add_fd(ev, fd, events, x11_event, x11);
	if (!x11->event_source) {
		wlr_log(WLR_ERROR, "Failed to create event source");
		goto error_xcb;
	}
	wl_event_source_check(x11->event_source);

	x11->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &x11->display_destroy);

	for (size_t i = 0; i < sizeof(atom_info) / sizeof(atom_info[0]); ++i) {
		xcb_intern_atom_reply_t *reply =
			xcb_intern_atom_reply(x11->xcb, atom_info[i].cookie, NULL);

		if (reply) {
			*atom_info[i].atom = reply->atom;
			free(reply);
		} else {
			*atom_info[i].atom = XCB_ATOM_NONE;
		}
	}

	const xcb_query_extension_reply_t *ext;
	// The X server requires us to query the extension version to use them
	xcb_dri3_query_version_cookie_t dri3_cookie;
	xcb_present_query_version_cookie_t present_cookie;
	xcb_xfixes_query_version_cookie_t xfixes_cookie;
	xcb_input_xi_query_version_cookie_t xinput_cookie;

	/*
	 * Query extension existence
	 */

	// TODO: Make optional when shm support is added
	ext = xcb_get_extension_data(x11->xcb, &xcb_dri3_id);
	if (!ext || !ext->present) {
		wlr_log(WLR_ERROR, "X11 server does not support DRI3 extension");
		goto error_exts;
	}
	dri3_cookie = xcb_dri3_query_version(x11->xcb, 1, 2);

	ext = xcb_get_extension_data(x11->xcb, &xcb_present_id);
	if (!ext || !ext->present) {
		wlr_log(WLR_ERROR, "X11 server does not support Present extension");
		goto error_exts;
	}
	x11->present_opcode = ext->major_opcode;
	present_cookie = xcb_present_query_version(x11->xcb, 1, 2);

	ext = xcb_get_extension_data(x11->xcb, &xcb_xfixes_id);
	if (!ext || !ext->present) {
		wlr_log(WLR_ERROR, "X11 server does not support Xfixes extension");
		goto error_exts;
	}
	xfixes_cookie = xcb_xfixes_query_version(x11->xcb, 4, 0);

	ext = xcb_get_extension_data(x11->xcb, &xcb_input_id);
	if (!ext || !ext->present) {
		wlr_log(WLR_ERROR, "X11 server does not support Xinput extension");
		goto error_exts;
	}
	x11->xinput_opcode = ext->major_opcode;
	xinput_cookie = xcb_input_xi_query_version(x11->xcb, 2, 0);

#if 0
//#ifdef WLR_HAS_XCB_XKB
	ext = xcb_get_extension_data(x11->xcb, &xcb_xkb_id);
	if (!ext || !ext->present) {
		wlr_log(WLR_ERROR, "X11 server does not support XKB extension");
	} else {
		x11->xkb_opcode = ext->major_opcode;

		xcb_xkb_use_extension_cookie_t cookie =
			xcb_xkb_use_extension(x11->xcb, 1, 0);
		xcb_xkb_use_extension_reply_t *reply =
			xcb_xkb_use_extension_reply(x11->xcb, cookie, NULL);
		if (reply && reply->supported) {
			x11->xkb_supported = true;

			xcb_xkb_select_events(x11->xcb, XCB_XKB_ID_USE_CORE_KBD,
				XCB_XKB_EVENT_TYPE_STATE_NOTIFY, 0,
				XCB_XKB_EVENT_TYPE_STATE_NOTIFY, 0, 0, NULL);
		}

		free(reply);
	}
#endif

	xcb_flush(x11->xcb);

	/*
	 * Query extension versions
	 */

	xcb_dri3_query_version_reply_t *dri3_reply = NULL;
	xcb_present_query_version_reply_t *present_reply = NULL;
	xcb_xfixes_query_version_reply_t *xfixes_reply = NULL;
	xcb_input_xi_query_version_reply_t *xinput_reply = NULL;

	dri3_reply = xcb_dri3_query_version_reply(x11->xcb, dri3_cookie, NULL);
	if (!dri3_reply || dri3_reply->major_version < 1) {
		wlr_log(WLR_ERROR, "X11 doesn't support required DRI3 version");
		goto error_version;
	}
	wlr_log(WLR_DEBUG, "X11 DRI3 version: %"PRIu32".%"PRIu32,
		dri3_reply->major_version, dri3_reply->minor_version);
	x11->has_modifiers =
		dri3_reply->major_version >= 1 && dri3_reply->minor_version >= 2;

	present_reply = xcb_present_query_version_reply(x11->xcb, present_cookie, NULL);
	if (!present_reply || present_reply->major_version < 1
			|| dri3_reply->minor_version < 2) {
		wlr_log(WLR_ERROR, "X11 doesn't support required Present version");
		goto error_version;
	}
	wlr_log(WLR_DEBUG, "X11 Present version: %"PRIu32".%"PRIu32,
		present_reply->major_version, present_reply->minor_version);

	xfixes_reply = xcb_xfixes_query_version_reply(x11->xcb, xfixes_cookie, NULL);
	if (!xfixes_reply || xfixes_reply->major_version < 4) {
		wlr_log(WLR_ERROR, "X11 doesn't support required Xfixes version");
		goto error_version;
	}
	wlr_log(WLR_DEBUG, "X11 Xfixes version: %"PRIu32".%"PRIu32,
		xfixes_reply->major_version, xfixes_reply->minor_version);

	xinput_reply = xcb_input_xi_query_version_reply(x11->xcb, xinput_cookie, NULL);
	if (!xinput_reply || xinput_reply->major_version < 2) {
		wlr_log(WLR_ERROR, "X11 doesn't support required Xinput version");
		goto error_version;
	}
	wlr_log(WLR_DEBUG, "X11 Xinput version: %"PRIu32".%"PRIu32,
		xinput_reply->major_version, xinput_reply->minor_version);

	free(dri3_reply);
	free(present_reply);
	free(xfixes_reply);
	free(xinput_reply);

	/*
	 * Open render node with DRI3
	 */

	xcb_dri3_open_cookie_t open_cookie =
		xcb_dri3_open(x11->xcb, x11->root, 0);
	xcb_dri3_open_reply_t *open_reply =
		xcb_dri3_open_reply(x11->xcb, open_cookie, NULL);

	if (!open_reply) {
		wlr_log(WLR_ERROR, "Failed to get DRI3 device");
		goto error_exts;
	}

	x11->render_fd = *xcb_dri3_open_reply_fds(x11->xcb, open_reply);
	free(open_reply);

	/*
	 * Get supported modifiers if any
	 */

	if (x11->has_modifiers) {
		xcb_dri3_get_supported_modifiers_cookie_t mod_cookie =
			xcb_dri3_get_supported_modifiers(x11->xcb, x11->root, 24, 32);
		xcb_dri3_get_supported_modifiers_reply_t *mod_reply =
			xcb_dri3_get_supported_modifiers_reply(x11->xcb, mod_cookie, NULL);

		uint64_t *mods = xcb_dri3_get_supported_modifiers_screen_modifiers(
			mod_reply);
		int n = xcb_dri3_get_supported_modifiers_screen_modifiers_length(
			mod_reply);

		for (int i = 0; i < n; ++i) {
			wlr_format_set_add(&x11->formats, DRM_FORMAT_XRGB8888,
				mods[i]);
		}

		if (n == 0) {
			wlr_format_set_add(&x11->formats, DRM_FORMAT_XRGB8888,
				DRM_FORMAT_MOD_LINEAR);
		}

		free(mod_reply);
	} else {
		wlr_format_set_add(&x11->formats, DRM_FORMAT_XRGB8888,
			DRM_FORMAT_MOD_LINEAR);
	}

	return &x11->backend;

error_version:
	free(dri3_reply);
	free(present_reply);
	free(xfixes_reply);
	free(xinput_reply);
error_exts:
	wlr_input_device_destroy(&x11->keyboard_dev);
	wl_event_source_remove(x11->event_source);
	wl_list_remove(&x11->display_destroy.link);
error_xcb:
	xcb_disconnect(x11->xcb);
error_x11:
	free(x11);
	return NULL;
}
