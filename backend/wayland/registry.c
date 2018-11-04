#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wlr/util/log.h>
#include <wlr/util/wlr_format_set.h>
#include "backend/wayland.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"

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

void poll_wl_registry(struct wlr_wl_backend *backend) {
	wl_registry_add_listener(backend->registry, &registry_listener, backend);
	wl_display_dispatch(backend->remote_display);
	wl_display_roundtrip(backend->remote_display);
}
