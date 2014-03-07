/*
 * Copyright © 2011-2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>

#include "egl_dri2.h"
#include "loader.h"

#include <wayland-client.h>
#include "wayland-drm-client-protocol.h"

#ifdef HAVE_LIBUDEV
#include <libudev.h>
#endif

#include "xmlconfig.h"
#include "xmlpool.h"


PUBLIC const char __driConfigOptionsWayland[] =
DRI_CONF_BEGIN
    DRI_CONF_SECTION_MISCELLANEOUS
        DRI_CONF_WANTED_DEVICE_ID_PATH_TAG()
    DRI_CONF_SECTION_END
DRI_CONF_END;

enum wl_drm_format_flags {
   HAS_ARGB8888 = 1,
   HAS_XRGB8888 = 2,
   HAS_RGB565 = 4,
};

static void
sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
   int *done = data;

   *done = 1;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
   sync_callback
};

static int
roundtrip(struct dri2_egl_display *dri2_dpy)
{
   struct wl_callback *callback;
   int done = 0, ret = 0;

   callback = wl_display_sync(dri2_dpy->wl_dpy);
   wl_callback_add_listener(callback, &sync_listener, &done);
   wl_proxy_set_queue((struct wl_proxy *) callback, dri2_dpy->wl_queue);
   while (ret != -1 && !done)
      ret = wl_display_dispatch_queue(dri2_dpy->wl_dpy, dri2_dpy->wl_queue);

   if (!done)
      wl_callback_destroy(callback);

   return ret;
}

static void
wl_buffer_release(void *data, struct wl_buffer *buffer)
{
   struct dri2_egl_surface *dri2_surf = data;
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); ++i)
      if (dri2_surf->color_buffers[i].wl_buffer == buffer)
         break;

   if (i == ARRAY_SIZE(dri2_surf->color_buffers)) {
      wl_buffer_destroy(buffer);
      return;
   }

   dri2_surf->color_buffers[i].locked = 0;
}

static struct wl_buffer_listener wl_buffer_listener = {
   wl_buffer_release
};

static void
resize_callback(struct wl_egl_window *wl_win, void *data)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);
}

/**
 * Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
 */
static _EGLSurface *
dri2_create_surface(_EGLDriver *drv, _EGLDisplay *disp, EGLint type,
		    _EGLConfig *conf, EGLNativeWindowType window,
		    const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;

   (void) drv;

   dri2_surf = malloc(sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }
   
   memset(dri2_surf, 0, sizeof *dri2_surf);
   if (!_eglInitSurface(&dri2_surf->base, disp, type, conf, attrib_list))
      goto cleanup_surf;

   if (conf->RedSize == 5)
      dri2_surf->format = WL_DRM_FORMAT_RGB565;
   else if (conf->AlphaSize == 0)
      dri2_surf->format = WL_DRM_FORMAT_XRGB8888;
   else
      dri2_surf->format = WL_DRM_FORMAT_ARGB8888;

   switch (type) {
   case EGL_WINDOW_BIT:
      dri2_surf->wl_win = (struct wl_egl_window *) window;

      dri2_surf->wl_win->private = dri2_surf;
      dri2_surf->wl_win->resize_callback = resize_callback;

      dri2_surf->base.Width =  -1;
      dri2_surf->base.Height = -1;
      break;
   default: 
      goto cleanup_surf;
   }

   dri2_surf->dri_drawable = 
      (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
					    type == EGL_WINDOW_BIT ?
					    dri2_conf->dri_double_config : 
					    dri2_conf->dri_single_config,
					    dri2_surf);
   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
      goto cleanup_dri_drawable;
   }

   return &dri2_surf->base;

 cleanup_dri_drawable:
   dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);
 cleanup_surf:
   free(dri2_surf);

   return NULL;
}

/**
 * Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
 */
static _EGLSurface *
dri2_create_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
			   _EGLConfig *conf, EGLNativeWindowType window,
			   const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLSurface *surf;

   surf = dri2_create_surface(drv, disp, EGL_WINDOW_BIT, conf,
			      window, attrib_list);

   if (surf != NULL)
      drv->API.SwapInterval(drv, disp, surf, dri2_dpy->default_swap_interval);

   return surf;
}

/**
 * Called via eglDestroySurface(), drv->API.DestroySurface().
 */
static EGLBoolean
dri2_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int i;

   (void) drv;

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wl_buffer)
         wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
   }

   for (i = 0; i < __DRI_BUFFER_COUNT; i++)
      if (dri2_surf->dri_buffers[i] &&
          dri2_surf->dri_buffers[i]->attachment != __DRI_BUFFER_BACK_LEFT)
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
                                       dri2_surf->dri_buffers[i]);

   if (dri2_surf->throttle_callback)
      wl_callback_destroy(dri2_surf->throttle_callback);

   if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
      dri2_surf->wl_win->private = NULL;
      dri2_surf->wl_win->resize_callback = NULL;
   }

   free(surf);

   return EGL_TRUE;
}

static void
dri2_release_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wl_buffer &&
          !dri2_surf->color_buffers[i].locked)
         wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);

      dri2_surf->color_buffers[i].wl_buffer = NULL;
      dri2_surf->color_buffers[i].dri_image = NULL;
      dri2_surf->color_buffers[i].locked = 0;
   }

   for (i = 0; i < __DRI_BUFFER_COUNT; i++)
      if (dri2_surf->dri_buffers[i] &&
          dri2_surf->dri_buffers[i]->attachment != __DRI_BUFFER_BACK_LEFT)
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
                                       dri2_surf->dri_buffers[i]);
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   /* We always want to throttle to some event (either a frame callback or
    * a sync request) after the commit so that we can be sure the
    * compositor has had a chance to handle it and send us a release event
    * before we look for a free buffer */
   while (dri2_surf->throttle_callback != NULL)
      if (wl_display_dispatch_queue(dri2_dpy->wl_dpy,
                                    dri2_dpy->wl_queue) == -1)
         return -1;

   if (dri2_surf->back == NULL) {
      for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         /* Get an unlocked buffer, preferrably one with a dri_buffer
          * already allocated. */
         if (dri2_surf->color_buffers[i].locked)
            continue;
         if (dri2_surf->back == NULL)
            dri2_surf->back = &dri2_surf->color_buffers[i];
         else if (dri2_surf->back->dri_image == NULL)
            dri2_surf->back = &dri2_surf->color_buffers[i];
      }
   }

   if (dri2_surf->back == NULL)
      return -1;
   if (dri2_surf->back->dri_image == NULL) {
      unsigned int use_flags = __DRI_IMAGE_USE_SHARE;

      if (!dri2_dpy->enable_tiling)
         use_flags |= __DRI_IMAGE_USE_LINEAR;

      dri2_surf->back->dri_image = 
         dri2_dpy->image->createImage(dri2_dpy->dri_screen,
                                      dri2_surf->base.Width,
                                      dri2_surf->base.Height,
                                      __DRI_IMAGE_FORMAT_ARGB8888,
                                      use_flags,
                                      NULL);
      dri2_surf->back->age = 0;
   }
   if (dri2_surf->back->dri_image == NULL)
      return -1;

   dri2_surf->back->locked = 1;

   return 0;
}


static void
back_bo_to_dri_buffer(struct dri2_egl_surface *dri2_surf, __DRIbuffer *buffer)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   __DRIimage *image;
   int name, pitch;

   image = dri2_surf->back->dri_image;

   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_NAME, &name);
   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_STRIDE, &pitch);

   buffer->attachment = __DRI_BUFFER_BACK_LEFT;
   buffer->name = name;
   buffer->pitch = pitch;
   buffer->cpp = 4;
   buffer->flags = 0;
}

static int
get_aux_bo(struct dri2_egl_surface *dri2_surf,
	   unsigned int attachment, unsigned int format, __DRIbuffer *buffer)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   __DRIbuffer *b = dri2_surf->dri_buffers[attachment];

   if (b == NULL) {
      b = dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen,
					 attachment, format,
					 dri2_surf->base.Width,
					 dri2_surf->base.Height);
      dri2_surf->dri_buffers[attachment] = b;
   }
   if (b == NULL)
      return -1;

   memcpy(buffer, b, sizeof *buffer);

   return 0;
}

static int
update_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width || 
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_release_buffers(dri2_surf);

      dri2_surf->base.Width  = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
   }

   if (get_back_bo(dri2_surf) < 0) {
      _eglError(EGL_BAD_ALLOC, "failed to allocate color buffer");
      return -1;
   }

   /* If we have an extra unlocked buffer at this point, we had to do triple
    * buffering for a while, but now can go back to just double buffering.
    * That means we can free any unlocked buffer now. */
   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (!dri2_surf->color_buffers[i].locked &&
          dri2_surf->color_buffers[i].wl_buffer) {
         wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
         dri2_surf->color_buffers[i].wl_buffer = NULL;
         dri2_surf->color_buffers[i].dri_image = NULL;
      }
   }

   return 0;
}

static __DRIbuffer *
dri2_get_buffers_with_format(__DRIdrawable * driDrawable,
			     int *width, int *height,
			     unsigned int *attachments, int count,
			     int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int i, j;

   if (update_buffers(dri2_surf) < 0)
      return NULL;

   for (i = 0, j = 0; i < 2 * count; i += 2, j++) {
      switch (attachments[i]) {
      case __DRI_BUFFER_BACK_LEFT:
         back_bo_to_dri_buffer(dri2_surf, &dri2_surf->buffers[j]);
	 break;
      default:
	 if (get_aux_bo(dri2_surf, attachments[i], attachments[i + 1],
			&dri2_surf->buffers[j]) < 0) {
	    _eglError(EGL_BAD_ALLOC, "failed to allocate aux buffer");
	    return NULL;
	 }
	 break;
      }
   }

   *out_count = j;
   if (j == 0)
	   return NULL;

   *width = dri2_surf->base.Width;
   *height = dri2_surf->base.Height;

   return dri2_surf->buffers;
}

static __DRIbuffer *
dri2_get_buffers(__DRIdrawable * driDrawable,
		 int *width, int *height,
		 unsigned int *attachments, int count,
		 int *out_count, void *loaderPrivate)
{
   unsigned int *attachments_with_format;
   __DRIbuffer *buffer;
   const unsigned int format = 32;
   int i;

   attachments_with_format = calloc(count * 2, sizeof(unsigned int));
   if (!attachments_with_format) {
      *out_count = 0;
      return NULL;
   }

   for (i = 0; i < count; ++i) {
      attachments_with_format[2*i] = attachments[i];
      attachments_with_format[2*i + 1] = format;
   }

   buffer =
      dri2_get_buffers_with_format(driDrawable,
				   width, height,
				   attachments_with_format, count,
				   out_count, loaderPrivate);

   free(attachments_with_format);

   return buffer;
}

static int
image_get_buffers(__DRIdrawable *driDrawable,
                  unsigned int format,
                  uint32_t *stamp,
                  void *loaderPrivate,
                  uint32_t buffer_mask,
                  struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (update_buffers(dri2_surf) < 0)
      return 0;

   buffers->image_mask = __DRI_IMAGE_BUFFER_BACK;
   buffers->back = dri2_surf->back->dri_image;

   return 1;
}

static void
dri2_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
   (void) driDrawable;
   (void) loaderPrivate;
}

static const __DRIimageLoaderExtension image_loader_extension = {
   .base = { __DRI_IMAGE_LOADER, 1 },

   .getBuffers          = image_get_buffers,
   .flushFrontBuffer    = dri2_flush_front_buffer,
};

static void
wayland_throttle_callback(void *data,
                          struct wl_callback *callback,
                          uint32_t time)
{
   struct dri2_egl_surface *dri2_surf = data;

   dri2_surf->throttle_callback = NULL;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener throttle_listener = {
   wayland_throttle_callback
};

static void
create_wl_buffer(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int fd, stride, name;

   if (dri2_surf->current->wl_buffer != NULL)
      return;

   if (dri2_dpy->capabilities & WL_DRM_CAPABILITY_PRIME) {
      dri2_dpy->image->queryImage(dri2_surf->current->dri_image,
                                  __DRI_IMAGE_ATTRIB_FD, &fd);
      dri2_dpy->image->queryImage(dri2_surf->current->dri_image,
                                  __DRI_IMAGE_ATTRIB_STRIDE, &stride);

      dri2_surf->current->wl_buffer =
         wl_drm_create_prime_buffer(dri2_dpy->wl_drm,
                                    fd,
                                    dri2_surf->base.Width,
                                    dri2_surf->base.Height,
                                    dri2_surf->format,
                                    0, stride,
                                    0, 0,
                                    0, 0);
      close(fd);
   } else {
      dri2_dpy->image->queryImage(dri2_surf->current->dri_image,
                                  __DRI_IMAGE_ATTRIB_NAME, &name);
      dri2_dpy->image->queryImage(dri2_surf->current->dri_image,
                                  __DRI_IMAGE_ATTRIB_STRIDE, &stride);

      dri2_surf->current->wl_buffer =
         wl_drm_create_buffer(dri2_dpy->wl_drm,
                              name,
                              dri2_surf->base.Width,
                              dri2_surf->base.Height,
                              stride,
                              dri2_surf->format);
   }

   wl_proxy_set_queue((struct wl_proxy *) dri2_surf->current->wl_buffer,
                      dri2_dpy->wl_queue);
   wl_buffer_add_listener(dri2_surf->current->wl_buffer,
                          &wl_buffer_listener, dri2_surf);
}

/**
 * Called via eglSwapBuffers(), drv->API.SwapBuffers().
 */
static EGLBoolean
dri2_swap_buffers_with_damage(_EGLDriver *drv,
                              _EGLDisplay *disp,
                              _EGLSurface *draw,
                              const EGLint *rects,
                              EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   struct dri2_egl_context *dri2_ctx;
   _EGLContext *ctx;
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++)
      if (dri2_surf->color_buffers[i].age > 0)
         dri2_surf->color_buffers[i].age++;

   /* Make sure we have a back buffer in case we're swapping without ever
    * rendering. */
   if (get_back_bo(dri2_surf) < 0) {
      _eglError(EGL_BAD_ALLOC, "dri2_swap_buffers");
      return EGL_FALSE;
   }

   if (draw->SwapInterval > 0) {
      dri2_surf->throttle_callback =
         wl_surface_frame(dri2_surf->wl_win->surface);
      wl_callback_add_listener(dri2_surf->throttle_callback,
                               &throttle_listener, dri2_surf);
      wl_proxy_set_queue((struct wl_proxy *) dri2_surf->throttle_callback,
                         dri2_dpy->wl_queue);
   }

   dri2_surf->back->age = 1;
   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   create_wl_buffer(dri2_surf);

   wl_surface_attach(dri2_surf->wl_win->surface,
                     dri2_surf->current->wl_buffer,
                     dri2_surf->dx, dri2_surf->dy);

   dri2_surf->wl_win->attached_width  = dri2_surf->base.Width;
   dri2_surf->wl_win->attached_height = dri2_surf->base.Height;
   /* reset resize growing parameters */
   dri2_surf->dx = 0;
   dri2_surf->dy = 0;

   if (n_rects == 0) {
      wl_surface_damage(dri2_surf->wl_win->surface,
                        0, 0, INT32_MAX, INT32_MAX);
   } else {
      for (i = 0; i < n_rects; i++) {
         const int *rect = &rects[i * 4];
         wl_surface_damage(dri2_surf->wl_win->surface,
                           rect[0],
                           dri2_surf->base.Height - rect[1] - rect[3],
                           rect[2], rect[3]);
      }
   }

   if (dri2_dpy->flush->base.version >= 4) {
      ctx = _eglGetCurrentContext();
      dri2_ctx = dri2_egl_context(ctx);
      (*dri2_dpy->flush->flush_with_flags)(dri2_ctx->dri_context,
                                           dri2_surf->dri_drawable,
                                           __DRI2_FLUSH_DRAWABLE,
                                           __DRI2_THROTTLE_SWAPBUFFER);
   } else {
      (*dri2_dpy->flush->flush)(dri2_surf->dri_drawable);
   }

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   wl_surface_commit(dri2_surf->wl_win->surface);

   /* If we're not waiting for a frame callback then we'll at least throttle
    * to a sync callback so that we always give a chance for the compositor to
    * handle the commit and send a release event before checking for a free
    * buffer */
   if (dri2_surf->throttle_callback == NULL) {
      dri2_surf->throttle_callback = wl_display_sync(dri2_dpy->wl_dpy);
      wl_callback_add_listener(dri2_surf->throttle_callback,
                               &throttle_listener, dri2_surf);
      wl_proxy_set_queue((struct wl_proxy *) dri2_surf->throttle_callback,
                         dri2_dpy->wl_queue);
   }

   wl_display_flush(dri2_dpy->wl_dpy);

   return EGL_TRUE;
}

static EGLint
dri2_query_buffer_age(_EGLDriver *drv,
                      _EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   if (get_back_bo(dri2_surf) < 0) {
      _eglError(EGL_BAD_ALLOC, "dri2_query_buffer_age");
      return 0;
   }

   return dri2_surf->back->age;
}

static EGLBoolean
dri2_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   return dri2_swap_buffers_with_damage (drv, disp, draw, NULL, 0);
}

static struct wl_buffer *
dri2_create_wayland_buffer_from_image_wl(_EGLDriver *drv,
                                         _EGLDisplay *disp,
                                         _EGLImage *img)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(img);
   __DRIimage *image = dri2_img->dri_image;
   struct wl_buffer *buffer;
   int width, height, format, pitch;
   enum wl_drm_format wl_format;

   /* The buffer to import likely has tiling. We can't check for it,
    * so assume we cannot import.*/
   if (!dri2_dpy->enable_tiling) {
      _eglError(EGL_BAD_MATCH, "dri2_create_wayland_buffer_from_image_wl");
      return NULL;
   }

   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_FORMAT, &format);

   switch (format) {
   case __DRI_IMAGE_FORMAT_ARGB8888:
      if (!(dri2_dpy->formats & HAS_ARGB8888))
         goto bad_format;
      wl_format = WL_DRM_FORMAT_ARGB8888;
      break;
   case __DRI_IMAGE_FORMAT_XRGB8888:
      if (!(dri2_dpy->formats & HAS_XRGB8888))
         goto bad_format;
      wl_format = WL_DRM_FORMAT_XRGB8888;
      break;
   default:
      goto bad_format;
   }

   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_WIDTH, &width);
   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_HEIGHT, &height);
   dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_STRIDE, &pitch);

   if (dri2_dpy->capabilities & WL_DRM_CAPABILITY_PRIME) {
      int fd;

      dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_FD, &fd);

      buffer =
         wl_drm_create_prime_buffer(dri2_dpy->wl_drm,
                                    fd,
                                    width, height,
                                    wl_format,
                                    0, pitch,
                                    0, 0,
                                    0, 0);

      close(fd);
   } else {
      int name;

      dri2_dpy->image->queryImage(image, __DRI_IMAGE_ATTRIB_NAME, &name);

      buffer =
         wl_drm_create_buffer(dri2_dpy->wl_drm,
                              name,
                              width, height,
                              pitch,
                              wl_format);
   }

   /* The buffer object will have been created with our internal event queue
    * because it is using the wl_drm object as a proxy factory. We want the
    * buffer to be used by the application so we'll reset it to the display's
    * default event queue */
   if (buffer)
      wl_proxy_set_queue((struct wl_proxy *) buffer, NULL);

   return buffer;

bad_format:
   _eglError(EGL_BAD_MATCH, "unsupported image format");
   return NULL;
}

static int
dri2_wayland_authenticate(_EGLDisplay *disp, uint32_t id)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int ret = 0;

   dri2_dpy->authenticated = 0;

   wl_drm_authenticate(dri2_dpy->wl_drm, id);
   if (roundtrip(dri2_dpy) < 0)
      ret = -1;

   if (!dri2_dpy->authenticated)
      ret = -1;

   /* reset authenticated */
   dri2_dpy->authenticated = 1;

   return ret;
}

/**
 * Called via eglTerminate(), drv->API.Terminate().
 */
static EGLBoolean
dri2_terminate(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   _eglReleaseDisplayResources(drv, disp);
   _eglCleanupDisplay(disp);

   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
   close(dri2_dpy->fd);
   dlclose(dri2_dpy->driver);
   free(dri2_dpy->driver_name);
   free(dri2_dpy->device_name);
   wl_drm_destroy(dri2_dpy->wl_drm);
   if (dri2_dpy->own_device)
      wl_display_disconnect(dri2_dpy->wl_dpy);
   free(dri2_dpy);
   disp->DriverData = NULL;

   return EGL_TRUE;
}

static char
is_fd_render_node(int fd)
{
   struct stat render;

   if (fstat(fd, &render))
      return 0;

   if (!S_ISCHR(render.st_mode))
      return 0;

   if (render.st_rdev & 0x80)
      return 1;
   return 0;
}

#ifdef HAVE_LIBUDEV

static char *
get_render_node_from_id_path_tag(struct udev *udev,
				 char *id_path_tag,
				 char another_tag)
{
   struct udev_device *device;
   struct udev_enumerate *e;
   struct udev_list_entry *entry;
   const char *path, *id_path_tag_tmp;
   char *path_res;
   char found = 0;

   e = udev_enumerate_new(udev);
   udev_enumerate_add_match_subsystem(e, "drm");
   udev_enumerate_add_match_sysname(e, "render*");

   udev_enumerate_scan_devices(e);
   udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
      path = udev_list_entry_get_name(entry);
      device = udev_device_new_from_syspath(udev, path);
      if (!device)
         continue;
      id_path_tag_tmp = udev_device_get_property_value(device, "ID_PATH_TAG");
      if (id_path_tag_tmp) {
         if ((!another_tag && !strcmp(id_path_tag, id_path_tag_tmp)) ||
             (another_tag && strcmp(id_path_tag, id_path_tag_tmp))) {
            found = 1;
            break;
         }
      }
      udev_device_unref(device);
   }

   if (found) {
      path_res = strdup(udev_device_get_devnode(device));
      udev_device_unref(device);
      return path_res;
   }
   return NULL;
}

static char *
get_id_path_tag_from_fd(struct udev *udev, int fd)
{
   struct udev_device *device;
   struct stat buf;
   const char *id_path_tag_tmp;

   if (fstat(fd, &buf) < 0) {
      return NULL;
   }

   device = udev_device_new_from_devnum(udev, 'c', buf.st_rdev);
   if (!device)
      return NULL;

   id_path_tag_tmp = udev_device_get_property_value(device, "ID_PATH_TAG");
   if (!id_path_tag_tmp)
      return NULL;

   return strdup(id_path_tag_tmp);
}
#endif

static EGLBoolean
is_render_node_capable(struct dri2_egl_display *dri2_dpy)
{
   const __DRIextension **extensions;
   int i;

   if (!(dri2_dpy->capabilities & WL_DRM_CAPABILITY_PRIME))
      return EGL_FALSE;

   extensions = dri2_dpy->driver_extensions;
   for (i = 0; extensions[i]; i++) {
      if (strcmp(extensions[i]->name, __DRI_IMAGE_DRIVER) == 0)
         return EGL_TRUE;
   }
   return EGL_FALSE;
}

static int
drm_open_device(const char *device_name)
{
   int fd;
#ifdef O_CLOEXEC
   fd = open(device_name, O_RDWR | O_CLOEXEC);
   if (fd == -1 && errno == EINVAL)
#endif
   {
      fd = open(device_name, O_RDWR);
      if (fd != -1)
         fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
   }
   return fd;
}

static void
drm_handle_device(void *data, struct wl_drm *drm, const char *device)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->device_name = strdup(device);
}

static void
drm_handle_format(void *data, struct wl_drm *drm, uint32_t format)
{
   struct dri2_egl_display *dri2_dpy = data;

   switch (format) {
   case WL_DRM_FORMAT_ARGB8888:
      dri2_dpy->formats |= HAS_ARGB8888;
      break;
   case WL_DRM_FORMAT_XRGB8888:
      dri2_dpy->formats |= HAS_XRGB8888;
      break;
   case WL_DRM_FORMAT_RGB565:
      dri2_dpy->formats |= HAS_RGB565;
      break;
   }
}

static void
drm_handle_capabilities(void *data, struct wl_drm *drm, uint32_t value)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->capabilities = value;
}

static void
drm_handle_authenticated(void *data, struct wl_drm *drm)
{
   struct dri2_egl_display *dri2_dpy = data;

   dri2_dpy->authenticated = 1;
}

static const struct wl_drm_listener drm_listener = {
	drm_handle_device,
	drm_handle_format,
	drm_handle_authenticated,
	drm_handle_capabilities
};

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
		       const char *interface, uint32_t version)
{
   struct dri2_egl_display *dri2_dpy = data;

   if (version > 1)
      version = 2;
   if (strcmp(interface, "wl_drm") == 0) {
      dri2_dpy->wl_drm =
         wl_registry_bind(registry, name, &wl_drm_interface, version);
      wl_drm_add_listener(dri2_dpy->wl_drm, &drm_listener, dri2_dpy);
   }
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
   registry_handle_global,
   registry_handle_global_remove
};

static EGLBoolean
dri2_swap_interval(_EGLDriver *drv,
                   _EGLDisplay *disp,
                   _EGLSurface *surf,
                   EGLint interval)
{
   if (interval > surf->Config->MaxSwapInterval)
      interval = surf->Config->MaxSwapInterval;
   else if (interval < surf->Config->MinSwapInterval)
      interval = surf->Config->MinSwapInterval;

   surf->SwapInterval = interval;

   return EGL_TRUE;
}

static void
dri2_setup_swap_interval(struct dri2_egl_display *dri2_dpy)
{
   GLint vblank_mode = DRI_CONF_VBLANK_DEF_INTERVAL_1;

   /* We can't use values greater than 1 on Wayland because we are using the
    * frame callback to synchronise the frame and the only way we be sure to
    * get a frame callback is to attach a new buffer. Therefore we can't just
    * sit drawing nothing to wait until the next ‘n’ frame callbacks */

   if (dri2_dpy->config)
      dri2_dpy->config->configQueryi(dri2_dpy->dri_screen,
                                     "vblank_mode", &vblank_mode);
   switch (vblank_mode) {
   case DRI_CONF_VBLANK_NEVER:
      dri2_dpy->min_swap_interval = 0;
      dri2_dpy->max_swap_interval = 0;
      dri2_dpy->default_swap_interval = 0;
      break;
   case DRI_CONF_VBLANK_ALWAYS_SYNC:
      dri2_dpy->min_swap_interval = 1;
      dri2_dpy->max_swap_interval = 1;
      dri2_dpy->default_swap_interval = 1;
      break;
   case DRI_CONF_VBLANK_DEF_INTERVAL_0:
      dri2_dpy->min_swap_interval = 0;
      dri2_dpy->max_swap_interval = 1;
      dri2_dpy->default_swap_interval = 0;
      break;
   default:
   case DRI_CONF_VBLANK_DEF_INTERVAL_1:
      dri2_dpy->min_swap_interval = 0;
      dri2_dpy->max_swap_interval = 1;
      dri2_dpy->default_swap_interval = 1;
      break;
   }
}

EGLBoolean
dri2_initialize_wayland(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   const __DRIconfig *config;
   uint32_t types;
   int i, is_render_node, device_fd, is_different_device;
   drm_magic_t magic;
   static const unsigned int argb_masks[4] =
      { 0xff0000, 0xff00, 0xff, 0xff000000 };
   static const unsigned int rgb_masks[4] = { 0xff0000, 0xff00, 0xff, 0 };
   static const unsigned int rgb565_masks[4] = { 0xf800, 0x07e0, 0x001f, 0 };

   driOptionCache defaultInitOptions;
   driOptionCache userInitOptions;
   char *prime_device_name = NULL;
   char *prime = NULL;
   const char *dri_prime = getenv("DRI_PRIME");

   if (dri_prime)
      prime = strdup(dri_prime);
   else {
      driParseOptionInfo(&defaultInitOptions, __driConfigOptionsWayland);
      driParseConfigFiles(&userInitOptions, &defaultInitOptions, 0, "init");
      if (driCheckOption(&userInitOptions, "wanted_device_id_path_tag",
                         DRI_STRING))
         prime = strdup(driQueryOptionstr(&userInitOptions,
                                          "wanted_device_id_path_tag"));
      driDestroyOptionCache(&userInitOptions);
      driDestroyOptionInfo(&defaultInitOptions);
   }

   loader_set_logger(_eglLog);

   drv->API.CreateWindowSurface = dri2_create_window_surface;
   drv->API.DestroySurface = dri2_destroy_surface;
   drv->API.SwapBuffers = dri2_swap_buffers;
   drv->API.SwapBuffersWithDamageEXT = dri2_swap_buffers_with_damage;
   drv->API.SwapInterval = dri2_swap_interval;
   drv->API.Terminate = dri2_terminate;
   drv->API.QueryBufferAge = dri2_query_buffer_age;

   drv->API.CreateWaylandBufferFromImageWL =
      dri2_create_wayland_buffer_from_image_wl;

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;
   if (disp->PlatformDisplay == NULL) {
      dri2_dpy->wl_dpy = wl_display_connect(NULL);
      if (dri2_dpy->wl_dpy == NULL)
         goto cleanup_dpy;
      dri2_dpy->own_device = 1;
   } else {
      dri2_dpy->wl_dpy = disp->PlatformDisplay;
   }

   dri2_dpy->wl_queue = wl_display_create_queue(dri2_dpy->wl_dpy);

   if (dri2_dpy->own_device)
      wl_display_dispatch_pending(dri2_dpy->wl_dpy);

   dri2_dpy->wl_registry = wl_display_get_registry(dri2_dpy->wl_dpy);
   wl_proxy_set_queue((struct wl_proxy *) dri2_dpy->wl_registry,
                      dri2_dpy->wl_queue);
   wl_registry_add_listener(dri2_dpy->wl_registry,
                            &registry_listener, dri2_dpy);
   if (roundtrip(dri2_dpy) < 0 || dri2_dpy->wl_drm == NULL)
      goto cleanup_dpy;

   if (roundtrip(dri2_dpy) < 0 || dri2_dpy->device_name == NULL)
      goto cleanup_drm;

   if (prime && !(dri2_dpy->capabilities & WL_DRM_CAPABILITY_PRIME)) {
   /* render-nodes are not supported */
      free(prime);
      prime = NULL;
   }

   device_fd = drm_open_device(dri2_dpy->device_name);
   if (device_fd == -1) {
      _eglLog(_EGL_WARNING, "wayland-egl: could not open %s (%s)",
	      dri2_dpy->device_name, strerror(errno));
      free(prime);
      goto cleanup_drm;
   }

   if (prime != NULL) {
#ifdef HAVE_LIBUDEV
      struct udev* udev = udev_new();
      char *device_id_path_tag;
      char another_tag = 0;

      if (!udev)
         goto prime_clean;
      device_id_path_tag = get_id_path_tag_from_fd(udev, device_fd);
      if (!device_id_path_tag)
         goto udev_clean;

      is_different_device = 1;
      /* two format are supported:
       * "1": choose any other card than the card used by the compositor.
       * id_path_tag: (for example "pci-0000_02_00_0") choose the card
       * with this id_path_tag. */
      if (!strcmp(prime,"1")) {
         free(prime);
         prime = strdup(device_id_path_tag);
         /* request a card with a different card than the compositor card */
         another_tag = 1;
      } else if (!strcmp(device_id_path_tag, prime))
         /* we want to get the render-node of the compositor card */
         is_different_device = 0;

      prime_device_name = get_render_node_from_id_path_tag(udev, prime,
                                                           another_tag);
      if (prime_device_name)
         _eglLog(_EGL_DEBUG,"requested device found: %s",
                  prime_device_name);
      else
         _eglLog(_EGL_WARNING,"requested device not found.");
      free(device_id_path_tag);
    udev_clean:
      udev_unref(udev);
    prime_clean:
#endif
      free(prime);
   }

   if (prime_device_name != NULL) {
      close(device_fd);
      free(dri2_dpy->device_name);
      dri2_dpy->device_name = prime_device_name;
      dri2_dpy->fd = drm_open_device(dri2_dpy->device_name);

      if (dri2_dpy->fd == -1) {
         _eglLog(_EGL_WARNING, "wayland-egl: could not open %s (%s)",
                 dri2_dpy->device_name, strerror(errno));
         goto cleanup_drm;
      }
   } else {
      is_different_device = 0;
      dri2_dpy->fd = device_fd;
   }

   if (is_fd_render_node(dri2_dpy->fd)) {
      _eglLog(_EGL_DEBUG, "wayland-egl: card is render-node");
      dri2_dpy->authenticated = 1;
      is_render_node = 1;
   } else {
      drmGetMagic(dri2_dpy->fd, &magic);
      wl_drm_authenticate(dri2_dpy->wl_drm, magic);
      is_render_node = 0;
   }
   dri2_dpy->enable_tiling = !is_different_device;

   if (roundtrip(dri2_dpy) < 0 || !dri2_dpy->authenticated)
      goto cleanup_fd;

   dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd, 0);
   if (dri2_dpy->driver_name == NULL) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to get driver name");
      goto cleanup_fd;
   }

   if (!dri2_load_driver(disp))
      goto cleanup_driver_name;

   dri2_dpy->dri2_loader_extension.base.name = __DRI_DRI2_LOADER;
   dri2_dpy->dri2_loader_extension.base.version = 3;
   dri2_dpy->dri2_loader_extension.getBuffers = dri2_get_buffers;
   dri2_dpy->dri2_loader_extension.flushFrontBuffer = dri2_flush_front_buffer;
   dri2_dpy->dri2_loader_extension.getBuffersWithFormat =
      dri2_get_buffers_with_format;

   dri2_dpy->extensions[0] = &dri2_dpy->dri2_loader_extension.base;
   dri2_dpy->extensions[1] = &image_loader_extension.base;
   dri2_dpy->extensions[2] = &image_lookup_extension.base;
   dri2_dpy->extensions[3] = &use_invalidate.base;
   dri2_dpy->extensions[4] = NULL;

   dri2_dpy->swap_available = EGL_TRUE;

   if (!dri2_create_screen(disp))
      goto cleanup_driver;

   dri2_setup_swap_interval(dri2_dpy);

   /* To use Prime, we must have _DRI_IMAGE v7 at least.
    * createImageFromFds support indicates that Prime export/import
    * is supported by the driver. Fall back to
    * gem names if we don't have Prime support. */

   if (dri2_dpy->image->base.version < 7 ||
       dri2_dpy->image->createImageFromFds == NULL)
      dri2_dpy->capabilities &= ~WL_DRM_CAPABILITY_PRIME;

   if (is_render_node && !is_render_node_capable(dri2_dpy)) {
      _eglLog(_EGL_WARNING, "wayland-egl: display is not render-node capable");
      goto cleanup_screen;
   }

   types = EGL_WINDOW_BIT;
   for (i = 0; dri2_dpy->driver_configs[i]; i++) {
      config = dri2_dpy->driver_configs[i];
      if (dri2_dpy->formats & HAS_XRGB8888)
	 dri2_add_config(disp, config, i + 1, types, NULL, rgb_masks);
      if (dri2_dpy->formats & HAS_ARGB8888)
	 dri2_add_config(disp, config, i + 1, types, NULL, argb_masks);
      if (dri2_dpy->formats & HAS_RGB565)
        dri2_add_config(disp, config, i + 1, types, NULL, rgb565_masks);
   }

   disp->Extensions.WL_bind_wayland_display = EGL_TRUE;
   disp->Extensions.WL_create_wayland_buffer_from_image = EGL_TRUE;
   disp->Extensions.EXT_buffer_age = EGL_TRUE;
   dri2_dpy->authenticate = dri2_wayland_authenticate;

   disp->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;

   /* we're supporting EGL 1.4 */
   disp->VersionMajor = 1;
   disp->VersionMinor = 4;

   return EGL_TRUE;
 cleanup_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
 cleanup_driver:
   dlclose(dri2_dpy->driver);
 cleanup_driver_name:
   free(dri2_dpy->driver_name);
 cleanup_fd:
   close(dri2_dpy->fd);
 cleanup_drm:
   free(dri2_dpy->device_name);
   wl_drm_destroy(dri2_dpy->wl_drm);
 cleanup_dpy:
   free(dri2_dpy);
   
   return EGL_FALSE;
}
