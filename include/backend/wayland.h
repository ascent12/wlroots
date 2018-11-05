#ifndef BACKEND_WAYLAND_H
#define BACKEND_WAYLAND_H

#include <stdbool.h>

#include <gbm.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-util.h>

#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_box.h>
#include <wlr/util/wlr_format_set.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-unstable-v6-client-protocol.h"

struct wlr_wl_backend {
	struct wlr_backend backend;

	int render_fd;

	/* local state */
	bool started;
	struct wl_display *local_display;
	struct wl_list devices;
	struct wl_list outputs;
	size_t requested_outputs;
	struct wl_listener local_display_destroy;

	/* remote state */
	struct wl_display *remote_display;
	struct wl_event_source *remote_display_src;
	struct wl_registry *registry;

	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	struct zxdg_shell_v6 *shell;
	struct wl_pointer *pointer;

	struct wlr_format_set formats;

	struct wlr_wl_pointer *current_pointer;
	char *seat_name;
};

struct wlr_wl_output {
	struct wlr_output wlr_output;

	struct wlr_wl_backend *backend;
	struct wl_list link;

	struct wl_surface *surface;
	struct wl_callback *frame_callback;
	struct zxdg_surface_v6 *xdg_surface;
	struct zxdg_toplevel_v6 *xdg_toplevel;

	struct gbm_bo *scheduled;

	uint32_t enter_serial;

	struct {
		struct wl_surface *surface;
		int32_t hotspot_x, hotspot_y;
		int32_t width, height;
	} cursor;
};

struct wlr_wl_buffer {
	struct wlr_wl_output *output;
	struct wl_buffer *buffer;
	void *userdata;
};

struct wlr_wl_input_device {
	struct wlr_input_device wlr_input_device;

	struct wlr_wl_backend *backend;
	void *resource;
};

struct wlr_wl_pointer {
	struct wlr_pointer wlr_pointer;

	struct wlr_wl_input_device *input_device;
	struct wl_pointer *wl_pointer;
	enum wlr_axis_source axis_source;
	int32_t axis_discrete;
	struct wlr_wl_output *output;

	struct wl_listener output_destroy;
};

struct wlr_wl_backend *get_wl_backend_from_backend(
	struct wlr_backend *wlr_backend);
void update_wl_output_cursor(struct wlr_wl_output *output);
struct wlr_wl_pointer *pointer_get_wl(struct wlr_pointer *wlr_pointer);
void create_wl_pointer(struct wl_pointer *wl_pointer,
	struct wlr_wl_output *output);

extern const struct wl_seat_listener seat_listener;

#endif
