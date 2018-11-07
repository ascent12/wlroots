#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/render/egl.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/render/wlr_swapchain.h>
#include <wlr/util/log.h>
#include "glapi.h"
#include "render/gles2.h"

static const struct wlr_renderer_impl renderer_impl;

static struct wlr_gles2_renderer *gles2_get_renderer(
		struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer->impl == &renderer_impl);
	return (struct wlr_gles2_renderer *)wlr_renderer;
}

static struct wlr_gles2_renderer *gles2_get_renderer_in_context(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	assert(wlr_egl_is_current(renderer->egl));
	return renderer;
}

static void gles2_begin(struct wlr_renderer *wlr_renderer, uint32_t width,
		uint32_t height) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	PUSH_GLES2_DEBUG;

	glViewport(0, 0, width, height);
	renderer->viewport_width = width;
	renderer->viewport_height = height;

	// enable transparency
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	// XXX: maybe we should save output projection and remove some of the need
	// for users to sling matricies themselves

	POP_GLES2_DEBUG;
}

static void gles2_end(struct wlr_renderer *wlr_renderer) {
	gles2_get_renderer_in_context(wlr_renderer);
	// no-op
}

static void gles2_clear(struct wlr_renderer *wlr_renderer,
		const float color[static 4]) {
	gles2_get_renderer_in_context(wlr_renderer);

	PUSH_GLES2_DEBUG;
	glClearColor(color[0], color[1], color[2], color[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	POP_GLES2_DEBUG;
}

static void gles2_scissor(struct wlr_renderer *wlr_renderer,
		struct wlr_box *box) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	PUSH_GLES2_DEBUG;
	if (box != NULL) {
		struct wlr_box gl_box;
		wlr_box_transform(box, WL_OUTPUT_TRANSFORM_FLIPPED_180,
			renderer->viewport_width, renderer->viewport_height, &gl_box);

		glScissor(gl_box.x, gl_box.y, gl_box.width, gl_box.height);
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	POP_GLES2_DEBUG;
}

static void draw_quad(void) {
	GLfloat verts[] = {
		1, 0, // top right
		0, 0, // top left
		1, 1, // bottom right
		0, 1, // bottom left
	};
	GLfloat texcoord[] = {
		1, 0, // top right
		0, 0, // top left
		1, 1, // bottom right
		0, 1, // bottom left
	};

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoord);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

static bool gles2_render_texture_with_matrix(struct wlr_renderer *wlr_renderer,
		struct wlr_texture *wlr_texture, const float matrix[static 9],
		float alpha) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);
	struct wlr_gles2_texture *texture =
		gles2_get_texture(wlr_texture);

	struct wlr_gles2_tex_shader *shader = NULL;
	GLenum target = 0;

	switch (texture->type) {
	case WLR_GLES2_TEXTURE_GLTEX:
	case WLR_GLES2_TEXTURE_WL_DRM_GL:
		if (texture->has_alpha) {
			shader = &renderer->shaders.tex_rgba;
		} else {
			shader = &renderer->shaders.tex_rgbx;
		}
		target = GL_TEXTURE_2D;
		break;
	case WLR_GLES2_TEXTURE_WL_DRM_EXT:
	case WLR_GLES2_TEXTURE_DMABUF:
		shader = &renderer->shaders.tex_ext;
		target = GL_TEXTURE_EXTERNAL_OES;

		if (!renderer->exts.egl_image_external_oes) {
			wlr_log(WLR_ERROR, "Failed to render texture: "
				"GL_TEXTURE_EXTERNAL_OES not supported");
			return false;
		}
		break;
	}

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	float transposition[9];
	wlr_matrix_transpose(transposition, matrix);

	PUSH_GLES2_DEBUG;

	GLuint tex_id = texture->type == WLR_GLES2_TEXTURE_GLTEX ?
		texture->gl_tex : texture->image_tex;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, tex_id);

	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glUseProgram(shader->program);

	glUniformMatrix3fv(shader->proj, 1, GL_FALSE, transposition);
	glUniform1i(shader->invert_y, texture->inverted_y);
	glUniform1i(shader->tex, 0);
	glUniform1f(shader->alpha, alpha);

	draw_quad();

	POP_GLES2_DEBUG;
	return true;
}

static void gles2_render_quad_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	float transposition[9];
	wlr_matrix_transpose(transposition, matrix);

	PUSH_GLES2_DEBUG;
	glUseProgram(renderer->shaders.quad.program);

	glUniformMatrix3fv(renderer->shaders.quad.proj, 1, GL_FALSE, transposition);
	glUniform4f(renderer->shaders.quad.color, color[0], color[1], color[2], color[3]);
	draw_quad();
	POP_GLES2_DEBUG;
}

static void gles2_render_ellipse_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	// OpenGL ES 2 requires the glUniformMatrix3fv transpose parameter to be set
	// to GL_FALSE
	float transposition[9];
	wlr_matrix_transpose(transposition, matrix);

	PUSH_GLES2_DEBUG;
	glUseProgram(renderer->shaders.ellipse.program);

	glUniformMatrix3fv(renderer->shaders.ellipse.proj, 1, GL_FALSE, transposition);
	glUniform4f(renderer->shaders.ellipse.color, color[0], color[1], color[2], color[3]);
	draw_quad();
	POP_GLES2_DEBUG;
}

static const enum wl_shm_format *gles2_renderer_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	return get_gles2_wl_formats(len);
}

static bool gles2_format_supported(struct wlr_renderer *wlr_renderer,
		enum wl_shm_format wl_fmt) {
	return get_gles2_format_from_wl(wl_fmt) != NULL;
}

static bool gles2_resource_is_wl_drm_buffer(struct wlr_renderer *wlr_renderer,
		struct wl_resource *resource) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);

	if (!eglQueryWaylandBufferWL) {
		return false;
	}

	EGLint fmt;
	return eglQueryWaylandBufferWL(renderer->egl->display, resource,
		EGL_TEXTURE_FORMAT, &fmt);
}

static void gles2_wl_drm_buffer_get_size(struct wlr_renderer *wlr_renderer,
		struct wl_resource *buffer, int *width, int *height) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);

	if (!eglQueryWaylandBufferWL) {
		return;
	}

	eglQueryWaylandBufferWL(renderer->egl->display, buffer, EGL_WIDTH, width);
	eglQueryWaylandBufferWL(renderer->egl->display, buffer, EGL_HEIGHT, height);
}

#if 0
static int gles2_get_dmabuf_formats(struct wlr_renderer *wlr_renderer,
		int **formats) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_formats(renderer->egl, formats);
}

static int gles2_get_dmabuf_modifiers(struct wlr_renderer *wlr_renderer,
		int format, uint64_t **modifiers) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_egl_get_dmabuf_modifiers(renderer->egl, format, modifiers);
}

static enum wl_shm_format gles2_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	GLint gl_format = -1, gl_type = -1;
	PUSH_GLES2_DEBUG;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &gl_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &gl_type);
	POP_GLES2_DEBUG;

	EGLint alpha_size = -1;
	eglGetConfigAttrib(renderer->egl->display, renderer->egl->config,
		EGL_ALPHA_SIZE, &alpha_size);

	const struct wlr_gles2_pixel_format *fmt =
		get_gles2_format_from_gl(gl_format, gl_type, alpha_size > 0);
	if (fmt != NULL) {
		return fmt->wl_format;
	}

	if (renderer->exts.read_format_bgra_ext) {
		return WL_SHM_FORMAT_XRGB8888;
	}
	return WL_SHM_FORMAT_XBGR8888;
}
#endif

static bool gles2_read_pixels(struct wlr_renderer *wlr_renderer,
		enum wl_shm_format wl_fmt, uint32_t *flags, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer_in_context(wlr_renderer);

	const struct wlr_gles2_pixel_format *fmt = get_gles2_format_from_wl(wl_fmt);
	if (fmt == NULL) {
		wlr_log(WLR_ERROR, "Cannot read pixels: unsupported pixel format");
		return false;
	}

	if (fmt->gl_format == GL_BGRA_EXT && !renderer->exts.read_format_bgra_ext) {
		wlr_log(WLR_ERROR,
			"Cannot read pixels: missing GL_EXT_read_format_bgra extension");
		return false;
	}

	PUSH_GLES2_DEBUG;

	// Make sure any pending drawing is finished before we try to read it
	glFinish();

	glGetError(); // Clear the error flag

	unsigned char *p = data + dst_y * stride;
	uint32_t pack_stride = width * fmt->bpp / 8;
	if (pack_stride == stride && dst_x == 0 && flags != NULL) {
		// Under these particular conditions, we can read the pixels with only
		// one glReadPixels call
		glReadPixels(src_x, renderer->viewport_height - height - src_y,
			width, height, fmt->gl_format, fmt->gl_type, p);
		*flags = WLR_RENDERER_READ_PIXELS_Y_INVERT;
	} else {
		// Unfortunately GLES2 doesn't support GL_PACK_*, so we have to read
		// the lines out row by row
		for (size_t i = src_y; i < src_y + height; ++i) {
			glReadPixels(src_x, src_y + height - i - 1, width, 1, fmt->gl_format,
				fmt->gl_type, p + i * stride + dst_x * fmt->bpp / 8);
		}
		if (flags != NULL) {
			*flags = 0;
		}
	}

	POP_GLES2_DEBUG;

	return glGetError() == GL_NO_ERROR;
}

static struct wlr_texture *gles2_texture_from_pixels(
		struct wlr_renderer *wlr_renderer, enum wl_shm_format wl_fmt,
		uint32_t stride, uint32_t width, uint32_t height, const void *data) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_gles2_texture_from_pixels(renderer->egl, wl_fmt, stride, width,
		height, data);
}

static struct wlr_texture *gles2_texture_from_wl_drm(
		struct wlr_renderer *wlr_renderer, struct wl_resource *data) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_gles2_texture_from_wl_drm(renderer->egl, data);
}

static struct wlr_texture *gles2_texture_from_dmabuf(
		struct wlr_renderer *wlr_renderer,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_gles2_renderer *renderer = gles2_get_renderer(wlr_renderer);
	return wlr_gles2_texture_from_dmabuf(renderer->egl, attribs);
}

static void gles2_init_wl_display(struct wlr_renderer *wlr_renderer,
		struct wl_display *wl_display) {
	struct wlr_gles2_renderer *renderer =
		gles2_get_renderer(wlr_renderer);
	if (!wlr_egl_bind_display(renderer->egl, wl_display)) {
		wlr_log(WLR_INFO, "failed to bind wl_display to EGL");
	}
}

static void gles2_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *gles = gles2_get_renderer(wlr_renderer);

	wlr_egl_make_current(gles->egl);

	PUSH_GLES2_DEBUG;
	glDeleteProgram(gles->shaders.quad.program);
	glDeleteProgram(gles->shaders.ellipse.program);
	glDeleteProgram(gles->shaders.tex_rgba.program);
	glDeleteProgram(gles->shaders.tex_rgbx.program);
	glDeleteProgram(gles->shaders.tex_ext.program);
	POP_GLES2_DEBUG;

#if 0
	if (renderer->exts.debug_khr) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		glDebugMessageCallbackKHR(NULL, NULL);
	}
#endif

	free(gles);
}

static void gles2_bind(struct wlr_renderer *renderer, struct wlr_image *img_base) {
	struct wlr_gles2_renderer *gles = gles2_get_renderer(renderer);
	struct wlr_gbm_image *img = (struct wlr_gbm_image *)img_base;
	struct wlr_gles2_image *priv = img->renderer_priv;

	wlr_egl_make_current(gles->egl);

	glBindFramebuffer(GL_FRAMEBUFFER, priv->framebuffer);
	glViewport(0, 0, gbm_bo_get_width(img->bo), gbm_bo_get_height(img->bo));
}

static void gles2_flush(struct wlr_renderer *renderer, int *fence_out) {
	if (fence_out) {
		//glFlush();
		glFinish();

		*fence_out = -1;
	} else {
		glFinish();
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static struct wlr_allocator *gles2_get_allocator(struct wlr_renderer *renderer) {
	struct wlr_gles2_renderer *gles = gles2_get_renderer(renderer);
	return &gles->gbm->base;
}

static const struct wlr_renderer_impl renderer_impl = {
	.get_allocator = gles2_get_allocator,
	.bind = gles2_bind,
	.flush = gles2_flush,
	.destroy = gles2_destroy,
	.begin = gles2_begin,
	.end = gles2_end,
	.clear = gles2_clear,
	.scissor = gles2_scissor,
	.render_texture_with_matrix = gles2_render_texture_with_matrix,
	.render_quad_with_matrix = gles2_render_quad_with_matrix,
	.render_ellipse_with_matrix = gles2_render_ellipse_with_matrix,
	.formats = gles2_renderer_formats,
	.format_supported = gles2_format_supported,
	.resource_is_wl_drm_buffer = gles2_resource_is_wl_drm_buffer,
	.wl_drm_buffer_get_size = gles2_wl_drm_buffer_get_size,
#if 0
	.get_dmabuf_formats = gles2_get_dmabuf_formats,
	.get_dmabuf_modifiers = gles2_get_dmabuf_modifiers,
	.preferred_read_format = gles2_preferred_read_format,
#endif
	.read_pixels = gles2_read_pixels,
	.texture_from_pixels = gles2_texture_from_pixels,
	.texture_from_wl_drm = gles2_texture_from_wl_drm,
	.texture_from_dmabuf = gles2_texture_from_dmabuf,
	.init_wl_display = gles2_init_wl_display,
};

void push_gles2_marker(const char *file, const char *func) {
	if (!glPushDebugGroupKHR) {
		return;
	}

	int len = snprintf(NULL, 0, "%s:%s", file, func) + 1;
	char str[len];
	snprintf(str, len, "%s:%s", file, func);
	glPushDebugGroupKHR(GL_DEBUG_SOURCE_APPLICATION_KHR, 1, -1, str);
}

void pop_gles2_marker(void) {
	if (glPopDebugGroupKHR) {
		glPopDebugGroupKHR();
	}
}

#if 0
static enum wlr_log_importance gles2_log_importance_to_wlr(GLenum type) {
	switch (type) {
	case GL_DEBUG_TYPE_ERROR_KHR:               return WLR_ERROR;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR: return WLR_DEBUG;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR:  return WLR_ERROR;
	case GL_DEBUG_TYPE_PORTABILITY_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_PERFORMANCE_KHR:         return WLR_DEBUG;
	case GL_DEBUG_TYPE_OTHER_KHR:               return WLR_DEBUG;
	case GL_DEBUG_TYPE_MARKER_KHR:              return WLR_DEBUG;
	case GL_DEBUG_TYPE_PUSH_GROUP_KHR:          return WLR_DEBUG;
	case GL_DEBUG_TYPE_POP_GROUP_KHR:           return WLR_DEBUG;
	default:                                    return WLR_DEBUG;
	}
}

static void gles2_log(GLenum src, GLenum type, GLuint id, GLenum severity,
		GLsizei len, const GLchar *msg, const void *user) {
	_wlr_log(gles2_log_importance_to_wlr(type), "[GLES2] %s", msg);
}
#endif

static GLuint compile_shader(GLuint type, const GLchar *src) {
	PUSH_GLES2_DEBUG;

	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteShader(shader);
		shader = 0;
	}

	POP_GLES2_DEBUG;
	return shader;
}

static GLuint link_program(const GLchar *vert_src, const GLchar *frag_src) {
	PUSH_GLES2_DEBUG;

	GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
	if (!vert) {
		goto error;
	}

	GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
	if (!frag) {
		glDeleteShader(vert);
		goto error;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vert);
	glAttachShader(prog, frag);
	glLinkProgram(prog);

	glDetachShader(prog, vert);
	glDetachShader(prog, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		glDeleteProgram(prog);
		goto error;
	}

	POP_GLES2_DEBUG;
	return prog;

error:
	POP_GLES2_DEBUG;
	return 0;
}

#if 0
static bool check_gl_ext(const char *exts, const char *ext) {
	size_t extlen = strlen(ext);
	const char *end = exts + strlen(exts);

	while (exts < end) {
		if (exts[0] == ' ') {
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
#endif

static bool gles2_gbm_create(void *data, struct wlr_gbm_image *img) {
	struct wlr_gles2_renderer *gles = data;

	struct wlr_gles2_image *priv = calloc(1, sizeof(*priv));
	if (!priv) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return false;
	}

	wlr_egl_make_current(gles->egl);

	priv->egl = wlr_egl_create_image(gles->egl, img->bo);
	if (!priv->egl) {
		free(priv);
		return false;
	}

	glGenFramebuffers(1, &priv->framebuffer);
	glGenRenderbuffers(1, &priv->renderbuffer);

	glBindRenderbuffer(GL_RENDERBUFFER, priv->renderbuffer);
	gles->egl_image_target_renderbuffer(GL_RENDERBUFFER, priv->egl);

	glBindFramebuffer(GL_FRAMEBUFFER, priv->framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_RENDERBUFFER, priv->renderbuffer);

	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	img->renderer_priv = priv;

	return true;
}

static bool gles2_gbm_destroy(void *data, struct wlr_gbm_image *img) {
	struct wlr_gles2_renderer *gles = data;
	struct wlr_gles2_image *priv = img->renderer_priv;

	glDeleteRenderbuffers(1, &priv->renderbuffer);
	glDeleteFramebuffers(1, &priv->framebuffer);
	wlr_egl_destroy_image(gles->egl, priv->egl);
	free(priv);

	return true;
}

extern const GLchar quad_vertex_src[];
extern const GLchar quad_fragment_src[];
extern const GLchar ellipse_fragment_src[];
extern const GLchar tex_vertex_src[];
extern const GLchar tex_fragment_src_rgba[];
extern const GLchar tex_fragment_src_rgbx[];
extern const GLchar tex_fragment_src_external[];

struct wlr_renderer *wlr_gles2_renderer_create(struct wlr_backend *backend) {
	struct wlr_gles2_renderer *gles = calloc(1, sizeof(*gles));
	if (!gles) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	gles->backend = backend;

	gles->gbm = wlr_gbm_allocator_create(backend, gles,
		gles2_gbm_create, gles2_gbm_destroy);
	if (!gles->gbm) {
		wlr_log_errno(WLR_ERROR, "Failed to create GBM device");
		return NULL;
	}

	gles->egl = wlr_egl_create(gles->gbm->gbm);
	if (!gles->egl) {
		return NULL;
	}

	wlr_egl_make_current(gles->egl);

	const char *exts = (const char *)glGetString(GL_EXTENSIONS);

	wlr_log(WLR_INFO, "Using %s", glGetString(GL_VERSION));
	wlr_log(WLR_INFO, "GL vendor: %s", glGetString(GL_VENDOR));
	wlr_log(WLR_INFO, "Supported GLES2 extensions: %s", exts);

	if (!strstr(exts, "OES_surfaceless_context")) {
		wlr_log(WLR_ERROR, "GLES does not support surfaceless contexts");
		return NULL;
	}

	if (!strstr(exts, "OES_EGL_image_external")) {
		wlr_log(WLR_ERROR, "GLES does not support EGL images");
		return NULL;
	}

	gles->egl_image_target_texture =
		(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	gles->egl_image_target_renderbuffer =
		(void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

	if (!strstr(exts, "GL_EXT_texture_format_BGRA8888")) {
		wlr_log(WLR_ERROR, "GLES does not support BGRA8888");
		return NULL;
	}

	wlr_renderer_init(&gles->wlr_renderer, &renderer_impl);

#if 0
	renderer->exts.read_format_bgra_ext =
		check_gl_ext(renderer->exts_str, "GL_EXT_read_format_bgra");
	renderer->exts.debug_khr =
		check_gl_ext(renderer->exts_str, "GL_KHR_debug") &&
		glDebugMessageCallbackKHR && glDebugMessageControlKHR;

	if (renderer->exts.debug_khr) {
		glEnable(GL_DEBUG_OUTPUT_KHR);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
		glDebugMessageCallbackKHR(gles2_log, NULL);

		// Silence unwanted message types
		glDebugMessageControlKHR(GL_DONT_CARE, GL_DEBUG_TYPE_POP_GROUP_KHR,
			GL_DONT_CARE, 0, NULL, GL_FALSE);
		glDebugMessageControlKHR(GL_DONT_CARE, GL_DEBUG_TYPE_PUSH_GROUP_KHR,
			GL_DONT_CARE, 0, NULL, GL_FALSE);
	}

	PUSH_GLES2_DEBUG;
#endif

	struct uniform {
		const char *name;
		GLint *loc;
	};

	struct {
		GLuint *prog;
		const char *vert_src;
		const char *frag_src;
		size_t num_uniforms;
		struct uniform *uniforms;
	} info[] = {
		{
			.prog = &gles->shaders.quad.program,
			.vert_src = quad_vertex_src,
			.frag_src = quad_fragment_src,
			.num_uniforms = 2,
			.uniforms = (struct uniform []) {
				{ "proj", &gles->shaders.quad.proj },
				{ "color", &gles->shaders.quad.color },
			},
		},
		{
			.prog = &gles->shaders.ellipse.program,
			.vert_src = quad_vertex_src,
			.frag_src = ellipse_fragment_src,
			.num_uniforms = 2,
			.uniforms = (struct uniform []) {
				{ "proj", &gles->shaders.ellipse.proj },
				{ "color", &gles->shaders.ellipse.color },
			},
		},
		{
			.prog = &gles->shaders.tex_rgba.program,
			.vert_src = tex_vertex_src,
			.frag_src = tex_fragment_src_rgba,
			.num_uniforms = 4,
			.uniforms = (struct uniform []) {
				{ "proj", &gles->shaders.tex_rgba.proj },
				{ "invert_y", &gles->shaders.tex_rgba.invert_y },
				{ "tex", &gles->shaders.tex_rgba.tex },
				{ "alpha", &gles->shaders.tex_rgba.alpha },
			},
		},
		{
			.prog = &gles->shaders.tex_rgbx.program,
			.vert_src = tex_vertex_src,
			.frag_src = tex_fragment_src_rgbx,
			.num_uniforms = 4,
			.uniforms = (struct uniform []) {
				{ "proj", &gles->shaders.tex_rgbx.proj },
				{ "invert_y", &gles->shaders.tex_rgbx.invert_y },
				{ "tex", &gles->shaders.tex_rgbx.tex },
				{ "alpha", &gles->shaders.tex_rgbx.alpha },
			},
		},
		{
			.prog = &gles->shaders.tex_ext.program,
			.vert_src = tex_vertex_src,
			.frag_src = tex_fragment_src_external,
			.num_uniforms = 4,
			.uniforms = (struct uniform []) {
				{ "proj", &gles->shaders.tex_ext.proj },
				{ "invert_y", &gles->shaders.tex_ext.invert_y },
				{ "tex", &gles->shaders.tex_ext.tex },
				{ "alpha", &gles->shaders.tex_ext.alpha },
			},
		},
	};

	for (size_t i = 0; i < sizeof(info) / sizeof(info[0]); ++i) {
		GLuint prog = link_program(info[i].vert_src, info[i].frag_src);
		if (!prog) {
			return NULL;
		}

		*info[i].prog = prog;

		for (size_t j = 0; j < info[i].num_uniforms; ++j) {
			struct uniform *u = &info[i].uniforms[j];
			*u->loc = glGetUniformLocation(prog, u->name);
		}
	}

#if 0
	POP_GLES2_DEBUG;
#endif

	return &gles->wlr_renderer;
#if 0
error:
	glDeleteProgram(renderer->shaders.quad.program);
	glDeleteProgram(renderer->shaders.ellipse.program);
	glDeleteProgram(renderer->shaders.tex_rgba.program);
	glDeleteProgram(renderer->shaders.tex_rgbx.program);
	glDeleteProgram(renderer->shaders.tex_ext.program);

	POP_GLES2_DEBUG;

	if (renderer->exts.debug_khr) {
		glDisable(GL_DEBUG_OUTPUT_KHR);
		glDebugMessageCallbackKHR(NULL, NULL);
	}

	free(renderer);
	return NULL;
#endif
}
