/* Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Note: the client API is licensed under ISC (see above) to ease
 * interoperability with other licenses. But keep in mind that the
 * mpv core is still mostly GPLv2+. It's up to lawyers to decide
 * whether applications using this API are affected by the GPL.
 * One argument against this is that proprietary applications
 * using mplayer in slave mode is apparently tolerated, and this
 * API is basically equivalent to slave mode.
 */

#ifndef MPV_CLIENT_API_OPENGL_CB_H_
#define MPV_CLIENT_API_OPENGL_CB_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Warning: this API is not stable yet.
 *
 * Overview
 * --------
 *
 * This API can be used to make mpv render into a foreign OpenGL context. It
 * can be used to handle video display. Be aware that using this API is not
 * required: you can embed the mpv window by setting the mpv "wid" option to
 * a native window handle (see "Embedding the video window" section in the
 * client.h header). In general, using the "wid" option is recommended over
 * the OpenGL API, because it's simpler and more flexible on the mpv side.
 *
 * The renderer needs to be explicitly initialized with mpv_opengl_cb_init_gl(),
 * and then video can be drawn with mpv_opengl_cb_render(). The user thread can
 * be notified by new frames with mpv_opengl_cb_set_update_callback().
 *
 * OpenGL interop
 * --------------
 *
 * This assumes the OpenGL context lives on a certain thread controlled by the
 * API user. The following functions require access to the OpenGL context:
 *      mpv_opengl_cb_init_gl
 *      mpv_opengl_cb_render
 *      mpv_opengl_cb_uninit_gl
 *
 * The OpenGL context is indirectly accessed through the OpenGL function
 * pointers returned by the get_proc_address callback in mpv_opengl_cb_init_gl.
 * Generally, mpv will not load the system OpenGL library when using this API.
 *
 * Only "desktop" OpenGL version 2.1 or later is supported. With OpenGL 2.1,
 * the GL_ARB_texture_rg is required. The renderer was written against
 * OpenGL 3.x core profile, with additional support for OpenGL 2.1.
 *
 * Note that some hardware decoding interop API (as set with the "hwdec" option)
 * may actually access
 *
 * OpenGL state
 * ------------
 *
 * OpenGL has a large amount of implicit state. All the mpv functions mentioned
 * above expect that the OpenGL state is reasonably set to OpenGL standard
 * defaults. Likewise, mpv will attempt to leave the OpenGL context with
 * standard defaults. The following state is excluded from this:
 *
 *      - the current viewport (can have/is set to an arbitrary value)
 *
 * Messing with the state could be avoided by creating shared OpenGL contexts,
 * but this is avoided for the sake of compatibility and interoperability.
 *
 * On OpenGL 2.1, mpv will strictly call functions like glGenTextures() to
 * create OpenGL objects. You will have to do the same. This ensures that
 * objects created by mpv and the API users don't clash.
 *
 * Threading
 * ---------
 *
 * The mpv_opengl_cb_* functions can be called from any thread, under the
 * following conditions:
 *  - only one of the mpv_opengl_cb_* functions can be called at the same time
 *    (unless they belong to different mpv_handles)
 *  - for functions which need an OpenGL context (see above) the OpenGL context
 *    must be "current" in the current thread, and it must be the same context
 *    as used with mpv_opengl_cb_init_gl()
 *  - never can be called from within the callbacks set with
 *    mpv_set_wakeup_callback() or mpv_opengl_cb_set_update_callback()
 *
 * Context and handle lifecycle
 * ----------------------------
 *
 * Video initialization will fail if the OpenGL context was not initialized yet
 * (with mpv_opengl_cb_init_gl()). Likewise, mpv_opengl_cb_uninit_gl() will
 * disable video.
 *
 * When the mpv core is destroyed (e.g. via mpv_terminate_destroy()), the OpenGL
 * context must have been uninitialized. If this doesn't happen, undefined
 * behavior will result.
 */

/**
 * Opaque context, returned by mpv_get_sub_api(MPV_SUB_API_OPENGL_CB).
 *
 * A context is bound to the mpv_handle it was retrieved from. The context
 * will always be the same (for the same mpv_handle), and is valid until the
 * mpv_handle it belongs to is released.
 */
typedef struct mpv_opengl_cb_context mpv_opengl_cb_context;

typedef void (*mpv_opengl_cb_update_fn)(void *cb_ctx);
typedef void *(*mpv_opengl_cb_get_proc_address_fn)(void *fn_ctx, const char *name);

/**
 * Set the callback that notifies you when a new video frame is available, or
 * if the video display configuration somehow changed and requires a redraw.
 * Similar to mpv_set_wakeup_callback(), you must not call any mpv API from
 * the callback.
 *
 * @param callback callback(callback_ctx) is called if the frame should be
 *                 redrawn
 * @param callback_ctx opaque argument to the callback
 */
void mpv_opengl_cb_set_update_callback(mpv_opengl_cb_context *ctx,
                                       mpv_opengl_cb_update_fn callback,
                                       void *callback_ctx);

/**
 * Initialize the mpv OpenGL state. This retrieves OpenGL function pointers via
 * get_proc_address, and creates OpenGL objects needed by mpv internally. It
 * will also call APIs needed for rendering hardware decoded video in OpenGL,
 * according to the mpv "hwdec" option.
 *
 * You must free the associated state at some point by calling the
 * mpv_opengl_cb_uninit_gl() function. Not doing so may result in memory leaks
 * or worse.
 *
 * @param exts optional _additional_ extension string, can be NULL
 * @param get_proc_address callback used to retrieve function pointers to OpenGL
 *                         functions. This is used for both standard functions
 *                         and extension functions. (The extension string is
 *                         checked whether extensions are really available.)
 *                         The callback will be called from this function only
 *                         (it is not stored and never used later).
 *                         Usually, GL context APIs do this for you (e.g. with
 *                         glXGetProcAddressARB or wglGetProcAddress), but
 *                         some APIs do not always return pointers for all
 *                         standard functions (even if present); in this case
 *                         you have to compensate by looking up these functions
 *                         yourself.
 * @param get_proc_address_ctx arbitrary opaque user context passed to the
 *                             get_proc_address callback
 * @return error code (same as normal mpv_* API), including but not limited to:
 *      MPV_ERROR_UNSUPPORTED: the OpenGL version is not supported
 *                             (or required extensions are missing)
 *      MPV_ERROR_INVALID_PARAMETER: the OpenGL state was already initialized
 */
int mpv_opengl_cb_init_gl(mpv_opengl_cb_context *ctx, const char *exts,
                          mpv_opengl_cb_get_proc_address_fn get_proc_address,
                          void *get_proc_address_ctx);

/**
 * Render video. Requires that the OpenGL state is initialized.
 *
 * The video will use the provided viewport rectangle as window size. Options
 * like "panscan" are applied to determine which part of the video should be
 * visible and how the video should be scaled. You can change these options
 * at runtime by using the mpv property API.
 *
 * The renderer will reconfigure itself every time the output rectangle/size
 * is changed. (If you want to do animations, it might be better to do the
 * animation on a FBO instead.)
 *
 * @param fbo The framebuffer object to render on. Because the renderer might
 *            manage multiple FBOs internally for the purpose of video
 *            postprocessing, it will always bind and unbind FBOs itself. If
 *            you want mpv to render on the main framebuffer, pass 0.
 * @param vp Viewport to render on. The renderer will essentially call:
 *            glViewport(vp[0], vp[1], vp[2], vp[3]);
 *           before rendering. The height (vp[3]) can be negative to flip the
 *           image - the renderer will flip it before setting the viewport
 *           (typically you want to flip the image if you are rendering
 *           directly to the main framebuffer).
 * @return the number of left frames in the internal queue to be rendered
 */
int mpv_opengl_cb_render(mpv_opengl_cb_context *ctx, int fbo, int vp[4]);

int mpv_opengl_cb_render_osd(struct mpv_opengl_cb_context *ctx, 
                             int w, int h, int ml, int mt, int mr, int mb, double dpar,
                             void(*cb)(void*,struct sub_bitmaps*), void *octx);

/**
 * Destroy the mpv OpenGL state.
 *
 * If video is still active (e.g. a file playing), video will be disabled
 * forcefully.
 *
 * Calling this multiple times is ok.
 *
 * @return error code
 */
int mpv_opengl_cb_uninit_gl(mpv_opengl_cb_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
