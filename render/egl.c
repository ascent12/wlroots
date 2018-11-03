#include <assert.h>
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdlib.h>
#include <drm_fourcc.h>
#include <wlr/render/egl.h>
#include <wlr/util/log.h>
#include "glapi.h"

#if 0
static bool egl_get_config(EGLDisplay disp, EGLint *attribs, EGLConfig *out,
		EGLint visual_id) {
	EGLint count = 0, matched = 0, ret;

	ret = eglGetConfigs(disp, NULL, 0, &count);
	if (ret == EGL_FALSE || count == 0) {
		wlr_log(WLR_ERROR, "eglGetConfigs returned no configs");
		return false;
	}

	EGLConfig configs[count];

	ret = eglChooseConfig(disp, attribs, configs, count, &matched);
	if (ret == EGL_FALSE) {
		wlr_log(WLR_ERROR, "eglChooseConfig failed");
		return false;
	}

	for (int i = 0; i < matched; ++i) {
		EGLint visual;
		if (!eglGetConfigAttrib(disp, configs[i],
				EGL_NATIVE_VISUAL_ID, &visual)) {
			continue;
		}

		if (!visual_id || visual == visual_id) {
			*out = configs[i];
			return true;
		}
	}

	wlr_log(WLR_ERROR, "no valid egl config found");
	return false;
}

static enum wlr_log_importance egl_log_importance_to_wlr(EGLint type) {
	switch (type) {
	case EGL_DEBUG_MSG_CRITICAL_KHR: return WLR_ERROR;
	case EGL_DEBUG_MSG_ERROR_KHR:    return WLR_ERROR;
	case EGL_DEBUG_MSG_WARN_KHR:     return WLR_ERROR;
	case EGL_DEBUG_MSG_INFO_KHR:     return WLR_INFO;
	default:                         return WLR_INFO;
	}
}

static void egl_log(EGLenum error, const char *command, EGLint msg_type,
		EGLLabelKHR thread, EGLLabelKHR obj, const char *msg) {
	_wlr_log(egl_log_importance_to_wlr(msg_type), "[EGL] %s: %s", command, msg);
}

static bool check_egl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (*exts == ' ') {
			exts++;
			continue;
		}
		size_t n = strcspn(exts, " ");
		if (n == extlen && strncmp(ext, exts, n) == 0) {
			return true;
		}
		exts += n;
	}
	return false;
}

static void print_dmabuf_formats(struct wlr_egl *egl) {
	/* Avoid log msg if extension is not present */
	if (!egl->exts.image_dmabuf_import_modifiers_ext) {
		return;
	}

	int *formats;
	int num = wlr_egl_get_dmabuf_formats(egl, &formats);
	if (num < 0) {
		return;
	}

	char str_formats[num * 5 + 1];
	for (int i = 0; i < num; i++) {
		snprintf(&str_formats[i*5], (num - i) * 5 + 1, "%.4s ",
			(char*)&formats[i]);
	}
	wlr_log(WLR_DEBUG, "Supported dmabuf buffer formats: %s", str_formats);
	free(formats);
}
#endif

struct wlr_egl *wlr_egl_create(struct gbm_device *gbm) {
	const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

	// This implies EGL_EXT_platform_base
	if (!strstr(client_exts, "EGL_MESA_platform_gbm")) {
		wlr_log(WLR_ERROR, "EGL does not support GBM platform");
		return NULL;
	}

	struct wlr_egl *egl = calloc(1, sizeof(*egl));
	if (!egl) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	egl->get_platform_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");

	egl->display = egl->get_platform_display(EGL_PLATFORM_GBM_MESA, gbm, NULL);
	if (!egl->display) {
		wlr_log(WLR_ERROR, "Failed to create EGL display");
		goto error_egl;
	}

	if (!eglInitialize(egl->display, NULL, NULL)) {
		wlr_log(WLR_ERROR, "Failed to initialize EGL");
		goto error_egl;
	}

	const char *instance_exts = eglQueryString(egl->display, EGL_EXTENSIONS);

	wlr_log(WLR_INFO, "Using EGL %s", eglQueryString(egl->display, EGL_VERSION));
	wlr_log(WLR_INFO, "EGL vendor: %s", eglQueryString(egl->display, EGL_VENDOR));
	wlr_log(WLR_INFO, "Supported EGL client extensions: %s", client_exts);
	wlr_log(WLR_INFO, "Supported EGL instance extensions: %s", instance_exts);

	// This implies EGL_KHR_image_base
	if (!strstr(instance_exts, "EGL_EXT_image_dma_buf_import")) {
		wlr_log(WLR_ERROR, "EGL does not support dmabuf import");
		goto error_display;
	}

	egl->has_modifiers = strstr(instance_exts, "EGL_EXT_image_dma_buf_import_modifiers");

	egl->create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
	egl->destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");

	if (!strstr(instance_exts, "EGL_KHR_surfaceless_context")) {
		wlr_log(WLR_ERROR, "EGL does not support surfaceless contexts");
		goto error_display;
	}

	if (!strstr(instance_exts, "EGL_KHR_no_config_context")) {
		wlr_log(WLR_ERROR, "EGL does not support configless contexts");
		goto error_display;
	}

	if (strstr(instance_exts, "EGL_WL_bind_wayland_display")) {
		egl->has_bind_wl = true;
		egl->bind_wl_display = (void *)eglGetProcAddress("eglBindWaylandDisplayWL");
		egl->unbind_wl_display = (void *)eglGetProcAddress("eglUnbindWaylandDisplayWL");
		egl->query_wl_buffer = (void *)eglGetProcAddress("eglQueryWaylandBufferWL");
	} else {
		wlr_log(WLR_ERROR, "EGL doesn't support binding wayland displays;"
			" some clients may be affected");
	}

	bool priority = strstr(instance_exts, "EGL_IMG_context_priority");

	EGLint attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG,
		EGL_NONE,
	};

	if (!priority) {
		attribs[2] = EGL_NONE;
	}

	egl->context = eglCreateContext(egl->display, EGL_NO_CONFIG_KHR,
		EGL_NO_CONTEXT, attribs);
	if (!egl->context) {
		wlr_log(WLR_ERROR, "Failed to create EGL context");
		goto error_display;
	}

	if (priority) {
		EGLint val;
		eglQueryContext(egl->display, egl->context,
			EGL_CONTEXT_PRIORITY_LEVEL_IMG, &val);

		if (val != EGL_CONTEXT_PRIORITY_HIGH_IMG) {
			wlr_log(WLR_ERROR, "Failed to create high priorty context");
			// Not an error
		}
	}

	return egl;

error_display:
	eglTerminate(egl->display);
error_egl:
	free(egl);
	return NULL;
}

void wlr_egl_destroy(struct wlr_egl *egl) {
	if (!egl) {
		return;
	}

	if (egl->wl_display) {
		assert(egl->has_bind_wl);
		egl->unbind_wl_display(egl->display, egl->wl_display);
	}

	eglDestroyContext(egl->display, egl->context);
	eglTerminate(egl->display);
	eglReleaseThread();
	free(egl);
}

bool wlr_egl_bind_display(struct wlr_egl *egl, struct wl_display *local_display) {
	if (!egl->has_bind_wl) {
		return false;
	}

	if (egl->wl_display) {
		egl->unbind_wl_display(egl->display, egl->wl_display);
		egl->wl_display = NULL;
	}

	if (egl->bind_wl_display(egl->display, local_display)) {
		egl->wl_display = local_display;
		return true;
	}

	return false;
}

bool wlr_egl_destroy_image(struct wlr_egl *egl, EGLImage image) {
	if (!image) {
		return true;
	}
	return egl->destroy_image(egl->display, image);
}

bool wlr_egl_make_current(struct wlr_egl *egl) {
	if (!eglMakeCurrent(egl->display, NULL, NULL, egl->context)) {
		wlr_log(WLR_ERROR, "eglMakeCurrent failed");
		return false;
	}
	return true;
}

bool wlr_egl_is_current(struct wlr_egl *egl) {
	return eglGetCurrentContext() == egl->context;
}

EGLImageKHR wlr_egl_create_image_from_wl_drm(struct wlr_egl *egl,
		struct wl_resource *data, EGLint *fmt, int *width, int *height,
		bool *inverted_y) {
	if (!egl->has_bind_wl) {
		return NULL;
	}

	if (!egl->query_wl_buffer(egl->display, data, EGL_TEXTURE_FORMAT, fmt)) {
		return NULL;
	}

	egl->query_wl_buffer(egl->display, data, EGL_WIDTH, width);
	egl->query_wl_buffer(egl->display, data, EGL_HEIGHT, height);

	EGLint _inverted_y;
	if (egl->query_wl_buffer(egl->display, data, EGL_WAYLAND_Y_INVERTED_WL,
			&_inverted_y)) {
		*inverted_y = (bool)_inverted_y;
	} else {
		*inverted_y = false;
	}

	const EGLint attribs[] = {
		EGL_WAYLAND_PLANE_WL, 0,
		EGL_NONE,
	};
	return egl->create_image(egl->display, egl->context, EGL_WAYLAND_BUFFER_WL,
		data, attribs);
}

EGLImageKHR wlr_egl_create_image_from_dmabuf(struct wlr_egl *egl,
		struct wlr_dmabuf_attributes *attributes) {
	bool has_modifier = false;

	// we assume the same way we assumed formats without the import_modifiers
	// extension that mod_linear is supported. The special mod mod_invalid
	// is sometimes used to signal modifier unawareness which is what we
	// have here
	if (attributes->modifier != DRM_FORMAT_MOD_INVALID &&
			attributes->modifier != DRM_FORMAT_MOD_LINEAR) {
		if (!egl->has_modifiers) {
			wlr_log(WLR_ERROR, "dmabuf modifiers extension not present");
			return NULL;
		}
		has_modifier = true;
	}

	unsigned int atti = 0;
	EGLint attribs[50];
	attribs[atti++] = EGL_WIDTH;
	attribs[atti++] = attributes->width;
	attribs[atti++] = EGL_HEIGHT;
	attribs[atti++] = attributes->height;
	attribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[atti++] = attributes->format;

	struct {
		EGLint fd;
		EGLint offset;
		EGLint pitch;
		EGLint mod_lo;
		EGLint mod_hi;
	} attr_names[WLR_DMABUF_MAX_PLANES] = {
		{
			EGL_DMA_BUF_PLANE0_FD_EXT,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,
			EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
		}, {
			EGL_DMA_BUF_PLANE1_FD_EXT,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT,
			EGL_DMA_BUF_PLANE1_PITCH_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT
		}, {
			EGL_DMA_BUF_PLANE2_FD_EXT,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT,
			EGL_DMA_BUF_PLANE2_PITCH_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT
		}, {
			EGL_DMA_BUF_PLANE3_FD_EXT,
			EGL_DMA_BUF_PLANE3_OFFSET_EXT,
			EGL_DMA_BUF_PLANE3_PITCH_EXT,
			EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
		}
	};

	for (int i = 0; i < attributes->n_planes; i++) {
		attribs[atti++] = attr_names[i].fd;
		attribs[atti++] = attributes->fd[i];
		attribs[atti++] = attr_names[i].offset;
		attribs[atti++] = attributes->offset[i];
		attribs[atti++] = attr_names[i].pitch;
		attribs[atti++] = attributes->stride[i];
		if (has_modifier) {
			attribs[atti++] = attr_names[i].mod_lo;
			attribs[atti++] = attributes->modifier & 0xFFFFFFFF;
			attribs[atti++] = attr_names[i].mod_hi;
			attribs[atti++] = attributes->modifier >> 32;
		}
	}
	attribs[atti++] = EGL_NONE;
	assert(atti < sizeof(attribs)/sizeof(attribs[0]));

	return egl->create_image(egl->display, EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
}

#if 0
int wlr_egl_get_dmabuf_formats(struct wlr_egl *egl,
		int **formats) {
	if (!egl->exts.image_dmabuf_import_ext) {
		wlr_log(WLR_DEBUG, "dmabuf import extension not present");
		return -1;
	}

	// when we only have the image_dmabuf_import extension we can't query
	// which formats are supported. These two are on almost always
	// supported; it's the intended way to just try to create buffers.
	// Just a guess but better than not supporting dmabufs at all,
	// given that the modifiers extension isn't supported everywhere.
	if (!egl->exts.image_dmabuf_import_modifiers_ext) {
		static const int fallback_formats[] = {
			DRM_FORMAT_ARGB8888,
			DRM_FORMAT_XRGB8888,
		};
		static unsigned num = sizeof(fallback_formats) /
			sizeof(fallback_formats[0]);

		*formats = calloc(num, sizeof(int));
		if (!*formats) {
			wlr_log_errno(WLR_ERROR, "Allocation failed");
			return -1;
		}

		memcpy(*formats, fallback_formats, num * sizeof(**formats));
		return num;
	}

	EGLint num;
	if (!eglQueryDmaBufFormatsEXT(egl->display, 0, NULL, &num)) {
		wlr_log(WLR_ERROR, "failed to query number of dmabuf formats");
		return -1;
	}

	*formats = calloc(num, sizeof(int));
	if (*formats == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed: %s", strerror(errno));
		return -1;
	}

	if (!eglQueryDmaBufFormatsEXT(egl->display, num, *formats, &num)) {
		wlr_log(WLR_ERROR, "failed to query dmabuf format");
		free(*formats);
		return -1;
	}
	return num;
}

int wlr_egl_get_dmabuf_modifiers(struct wlr_egl *egl,
		int format, uint64_t **modifiers) {
	if (!egl->exts.image_dmabuf_import_ext) {
		wlr_log(WLR_DEBUG, "dmabuf extension not present");
		return -1;
	}

	if(!egl->exts.image_dmabuf_import_modifiers_ext) {
		*modifiers = NULL;
		return 0;
	}

	EGLint num;
	if (!eglQueryDmaBufModifiersEXT(egl->display, format, 0,
			NULL, NULL, &num)) {
		wlr_log(WLR_ERROR, "failed to query dmabuf number of modifiers");
		return -1;
	}

	*modifiers = calloc(num, sizeof(uint64_t));
	if (*modifiers == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed: %s", strerror(errno));
		return -1;
	}

	if (!eglQueryDmaBufModifiersEXT(egl->display, format, num,
		*modifiers, NULL, &num)) {
		wlr_log(WLR_ERROR, "failed to query dmabuf modifiers");
		free(*modifiers);
		return -1;
	}
	return num;
}

bool wlr_egl_export_image_to_dmabuf(struct wlr_egl *egl, EGLImageKHR image,
		int32_t width, int32_t height, uint32_t flags,
		struct wlr_dmabuf_attributes *attribs) {
	memset(attribs, 0, sizeof(struct wlr_dmabuf_attributes));

	if (!egl->exts.image_dma_buf_export_mesa) {
		return false;
	}

	// Only one set of modifiers is returned for all planes
	if (!eglExportDMABUFImageQueryMESA(egl->display, image,
			(int *)&attribs->format, &attribs->n_planes, &attribs->modifier)) {
		return false;
	}
	if (attribs->n_planes > WLR_DMABUF_MAX_PLANES) {
		wlr_log(WLR_ERROR, "EGL returned %d planes, but only %d are supported",
			attribs->n_planes, WLR_DMABUF_MAX_PLANES);
		return false;
	}

	if (!eglExportDMABUFImageMESA(egl->display, image, attribs->fd,
			(EGLint *)attribs->stride, (EGLint *)attribs->offset)) {
		return false;
	}

	attribs->width = width;
	attribs->height = height;
	attribs->flags = flags;
	return true;
}
#endif
