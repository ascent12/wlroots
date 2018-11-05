#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <wayland-server.h>

#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/util/log.h>

#include "backend/wayland.h"
#include "util/signal.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"

struct wlr_wl_backend *get_wl_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_wl(wlr_backend));
	return (struct wlr_wl_backend *)wlr_backend;
}

static int dispatch_events(int fd, uint32_t mask, void *data) {
	struct wlr_wl_backend *wl = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wl_display_terminate(wl->local_display);
	} else {
		wl_display_dispatch(wl->remote_display);
	}

	return 0;
}

static bool backend_start(struct wlr_backend *backend) {
	struct wlr_wl_backend *wl = get_wl_backend_from_backend(backend);

	wlr_log(WLR_INFO, "Initializating wayland backend");

	wl->started = true;

	for (size_t i = 0; i < wl->requested_outputs; ++i) {
		wlr_wl_output_create(&wl->backend);
	}

	wl->requested_outputs = 0;
	return true;
}

static void backend_destroy(struct wlr_backend *backend) {
	struct wlr_wl_backend *wl = get_wl_backend_from_backend(backend);
	if (!wl) {
		return;
	}

	struct wlr_wl_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &wl->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	struct wlr_input_device *input_device, *tmp_input;
	wl_list_for_each_safe(input_device, tmp_input, &wl->devices, link) {
		wlr_input_device_destroy(input_device);
	}

	wlr_signal_emit_safe(&backend->events.destroy, backend);
	wl_list_remove(&wl->local_display_destroy.link);
	free(wl->seat_name);

	if (wl->pointer) {
		wl_pointer_destroy(wl->pointer);
	}
	if (wl->seat) {
		wl_seat_destroy(wl->seat);
	}
	wl_compositor_destroy(wl->compositor);
	zwp_linux_dmabuf_v1_destroy(wl->dmabuf);
	zxdg_shell_v6_destroy(wl->shell);

	wl_registry_destroy(wl->registry);
	wl_event_source_remove(wl->remote_display_src);
	wl_display_disconnect(wl->remote_display);
	close(wl->render_fd);
	free(wl);
}

static int backend_get_render_fd(struct wlr_backend *backend) {
	struct wlr_wl_backend *wl = get_wl_backend_from_backend(backend);
	return wl->render_fd;
}

static struct wlr_format_set *backend_get_formats(struct wlr_backend *backend) {
	struct wlr_wl_backend *wl = get_wl_backend_from_backend(backend);
	return &wl->formats;
}

static struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_render_fd = backend_get_render_fd,
	.get_formats = backend_get_formats,
};

bool wlr_backend_is_wl(struct wlr_backend *b) {
	return b->impl == &backend_impl;
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_wl_backend *wl =
		wl_container_of(listener, wl, local_display_destroy);
	backend_destroy(&wl->backend);
}

static void dmabuf_handle_format(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
		uint32_t format) {
	struct wlr_wl_backend *wl = data;

	if (!wlr_format_set_add(&wl->formats, format, DRM_FORMAT_MOD_INVALID)) {
		wlr_log(WLR_ERROR, "Failed to add format %#"PRIx32, format);
	}
}

static void dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *dmabuf,
		uint32_t format, uint32_t mod_high, uint32_t mod_low) {
	struct wlr_wl_backend *wl = data;
	uint64_t mod = ((uint64_t)mod_high << 32) | mod_low;

	if (!wlr_format_set_add(&wl->formats, format, mod)) {
		wlr_log(WLR_ERROR, "Failed to add format %#"PRIx32" (%#"PRIx64")",
			format, mod);
	}
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	.format = dmabuf_handle_format,
	.modifier = dmabuf_handle_modifier,
};

static void xdg_shell_handle_ping(void *data, struct zxdg_shell_v6 *shell,
		uint32_t serial) {
	zxdg_shell_v6_pong(shell, serial);
}

static const struct zxdg_shell_v6_listener xdg_shell_listener = {
	.ping = xdg_shell_handle_ping,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *iface, uint32_t version) {
	struct wlr_wl_backend *wl = data;

	wlr_log(WLR_DEBUG, "Remote wayland global: %s v%"PRIu32, iface, version);

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		wl->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 1);

	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(wl->seat, &seat_listener, wl);

	} else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		wl->dmabuf = wl_registry_bind(registry, name,
			&zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(wl->dmabuf, &dmabuf_listener, wl);

	} else if (strcmp(iface, zxdg_shell_v6_interface.name) == 0) {
		wl->shell = wl_registry_bind(registry, name,
			&zxdg_shell_v6_interface, 1);
		zxdg_shell_v6_add_listener(wl->shell, &xdg_shell_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// TODO
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove
};

struct wlr_backend *wlr_wl_backend_create(struct wl_display *display, const char *remote) {
	wlr_log(WLR_INFO, "Creating wayland backend");

	struct wlr_wl_backend *wl = calloc(1, sizeof(*wl));
	if (!wl) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	wlr_backend_init(&wl->backend, &backend_impl);

	wl->render_fd = open("/dev/dri/renderD128", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (wl->render_fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to open render node");
		goto error_wl;
	}

	wl->local_display = display;
	wl_list_init(&wl->devices);
	wl_list_init(&wl->outputs);

	wl->remote_display = wl_display_connect(remote);
	if (!wl->remote_display) {
		wlr_log_errno(WLR_ERROR, "Could not connect to remote display");
		goto error_render;
	}

	wl->registry = wl_display_get_registry(wl->remote_display);
	if (!wl->registry) {
		wlr_log_errno(WLR_ERROR, "Could not obtain reference to remote registry");
		goto error_remote;
	}

	wl_registry_add_listener(wl->registry, &registry_listener, wl);

	// Collect wayland globals, including dmabuf formats
	wl_display_dispatch(wl->remote_display);
	wl_display_roundtrip(wl->remote_display);

	if (!wl->compositor) {
		wlr_log(WLR_ERROR,
			"Remote wayland compositor does not support wl_compositor");
		goto error_globals;
	}
	if (!wl->dmabuf) {
		wlr_log(WLR_ERROR,
			"Remote wayland compositor does not support zwp_linux_dmabuf_v1");
		goto error_globals;
	}
	if (!wl->shell) {
		wlr_log(WLR_ERROR,
			"Remote wayland compositor does not support zxdg_shell_v6");
		goto error_globals;
	}

	wl->local_display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(wl->local_display, &wl->local_display_destroy);

	struct wl_event_loop *loop = wl_display_get_event_loop(wl->local_display);
	int fd = wl_display_get_fd(wl->remote_display);
	int events = WL_EVENT_WRITABLE | WL_EVENT_READABLE | WL_EVENT_ERROR | WL_EVENT_HANGUP;

	wl->remote_display_src = wl_event_loop_add_fd(loop, fd, events,
		dispatch_events, wl);
	if (!wl->remote_display_src) {
		wlr_log_errno(WLR_ERROR, "Failed to create event source");
		goto error_globals;
	}

	return &wl->backend;

error_globals:
	if (wl->compositor) {
		wl_compositor_destroy(wl->compositor);
	}
	if (wl->dmabuf) {
		zwp_linux_dmabuf_v1_destroy(wl->dmabuf);
	}
	if (wl->shell) {
		zxdg_shell_v6_destroy(wl->shell);
	}
	wl_registry_destroy(wl->registry);
error_remote:
	wl_display_disconnect(wl->remote_display);
error_render:
	close(wl->render_fd);
error_wl:
	free(wl);
	return NULL;
}
