#ifndef WLR_UTIL_WLR_FORMAT_SET_H
#define WLR_UTIL_WLR_FORMAT_SET_H

struct wlr_format {
	uint32_t format;
	size_t cap;
	size_t len;
	uint64_t modifiers[];
};

struct wlr_format_set {
	size_t cap;
	size_t len;
	struct wlr_format **formats;
};

void wlr_format_set_release(struct wlr_format_set *set);

struct wlr_format *wlr_format_set_get(struct wlr_format_set *set, uint32_t format);

bool wlr_format_set_add(struct wlr_format_set *set, uint32_t format,
	uint64_t modifier);

#endif
