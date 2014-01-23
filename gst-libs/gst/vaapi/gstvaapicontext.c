/*
 *  gstvaapicontext.c - VA context abstraction
 *
 *  Copyright (C) 2010-2011 Splitted-Desktop Systems
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@splitted-desktop.com>
 *  Copyright (C) 2011-2014 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

/**
 * SECTION:gstvaapicontext
 * @short_description: VA context abstraction
 */

#include "sysdeps.h"
#include "gstvaapicompat.h"
#include "gstvaapicontext.h"
#include "gstvaapicontext_overlay.h"
#include "gstvaapidisplay_priv.h"
#include "gstvaapiobject_priv.h"
#include "gstvaapisurface.h"
#include "gstvaapisurface_priv.h"
#include "gstvaapisurfacepool.h"
#include "gstvaapisurfaceproxy.h"
#include "gstvaapivideopool_priv.h"
#include "gstvaapiutils.h"

#define DEBUG 1
#include "gstvaapidebug.h"

static void
unref_surface_cb (GstVaapiSurface * surface)
{
  gst_vaapi_surface_set_parent_context (surface, NULL);
  gst_vaapi_object_unref (surface);
}

static void
context_destroy_surfaces (GstVaapiContext * context)
{
  gst_vaapi_context_overlay_reset (context);

  if (context->surfaces) {
    g_ptr_array_unref (context->surfaces);
    context->surfaces = NULL;
  }
  gst_vaapi_video_pool_replace (&context->surfaces_pool, NULL);
}

static void
context_destroy (GstVaapiContext * context)
{
  GstVaapiDisplay *const display = GST_VAAPI_OBJECT_DISPLAY (context);
  VAContextID context_id;
  VAStatus status;

  context_id = GST_VAAPI_OBJECT_ID (context);
  GST_DEBUG ("context 0x%08x", context_id);

  if (context_id != VA_INVALID_ID) {
    GST_VAAPI_DISPLAY_LOCK (display);
    status = vaDestroyContext (GST_VAAPI_DISPLAY_VADISPLAY (display),
        context_id);
    GST_VAAPI_DISPLAY_UNLOCK (display);
    if (!vaapi_check_status (status, "vaDestroyContext()"))
      GST_WARNING ("failed to destroy context 0x%08x", context_id);
    GST_VAAPI_OBJECT_ID (context) = VA_INVALID_ID;
  }

  if (context->va_config != VA_INVALID_ID) {
    GST_VAAPI_DISPLAY_LOCK (display);
    status = vaDestroyConfig (GST_VAAPI_DISPLAY_VADISPLAY (display),
        context->va_config);
    GST_VAAPI_DISPLAY_UNLOCK (display);
    if (!vaapi_check_status (status, "vaDestroyConfig()"))
      GST_WARNING ("failed to destroy config 0x%08x", context->va_config);
    context->va_config = VA_INVALID_ID;
  }
}

static gboolean
context_create_surfaces (GstVaapiContext * context)
{
  const GstVaapiContextInfo *const cip = &context->info;
  GstVideoInfo vi;
  GstVaapiSurface *surface;
  guint i, num_surfaces;

  /* Number of scratch surfaces beyond those used as reference */
  const guint SCRATCH_SURFACES_COUNT = 4;

  if (!gst_vaapi_context_overlay_reset (context))
    return FALSE;

  num_surfaces = cip->ref_frames + SCRATCH_SURFACES_COUNT;
  if (!context->surfaces) {
    context->surfaces = g_ptr_array_new_full (num_surfaces,
        (GDestroyNotify) unref_surface_cb);
    if (!context->surfaces)
      return FALSE;
  }

  if (!context->surfaces_pool) {
    gst_video_info_set_format (&vi, GST_VIDEO_FORMAT_ENCODED,
        cip->width, cip->height);
    context->surfaces_pool =
        gst_vaapi_surface_pool_new (GST_VAAPI_OBJECT_DISPLAY (context), &vi);
    if (!context->surfaces_pool)
      return FALSE;
  }
  gst_vaapi_video_pool_set_capacity (context->surfaces_pool, num_surfaces);

  for (i = context->surfaces->len; i < num_surfaces; i++) {
    surface = gst_vaapi_surface_new (GST_VAAPI_OBJECT_DISPLAY (context),
        GST_VAAPI_CHROMA_TYPE_YUV420, cip->width, cip->height);
    if (!surface)
      return FALSE;
    gst_vaapi_surface_set_parent_context (surface, context);
    g_ptr_array_add (context->surfaces, surface);
    if (!gst_vaapi_video_pool_add_object (context->surfaces_pool, surface))
      return FALSE;
  }
  return TRUE;
}

static gboolean
context_create (GstVaapiContext * context)
{
  const GstVaapiContextInfo *const cip = &context->info;
  GstVaapiDisplay *const display = GST_VAAPI_OBJECT_DISPLAY (context);
  guint va_rate_control;
  VAConfigAttrib attribs[2];
  guint num_attribs;
  VAContextID context_id;
  VASurfaceID surface_id;
  VAStatus status;
  GArray *surfaces = NULL;
  gboolean success = FALSE;
  guint i;

  if (!context->surfaces && !context_create_surfaces (context))
    goto cleanup;

  surfaces = g_array_sized_new (FALSE,
      FALSE, sizeof (VASurfaceID), context->surfaces->len);
  if (!surfaces)
    goto cleanup;

  for (i = 0; i < context->surfaces->len; i++) {
    GstVaapiSurface *const surface = g_ptr_array_index (context->surfaces, i);
    if (!surface)
      goto cleanup;
    surface_id = GST_VAAPI_OBJECT_ID (surface);
    g_array_append_val (surfaces, surface_id);
  }
  g_assert (surfaces->len == context->surfaces->len);

  if (!cip->profile || !cip->entrypoint)
    goto cleanup;
  context->va_profile = gst_vaapi_profile_get_va_profile (cip->profile);
  context->va_entrypoint =
      gst_vaapi_entrypoint_get_va_entrypoint (cip->entrypoint);

  num_attribs = 0;
  attribs[num_attribs++].type = VAConfigAttribRTFormat;
  if (cip->entrypoint == GST_VAAPI_ENTRYPOINT_SLICE_ENCODE)
    attribs[num_attribs++].type = VAConfigAttribRateControl;

  GST_VAAPI_DISPLAY_LOCK (display);
  status = vaGetConfigAttributes (GST_VAAPI_DISPLAY_VADISPLAY (display),
      context->va_profile, context->va_entrypoint, attribs, num_attribs);
  GST_VAAPI_DISPLAY_UNLOCK (display);
  if (!vaapi_check_status (status, "vaGetConfigAttributes()"))
    goto cleanup;
  if (!(attribs[0].value & VA_RT_FORMAT_YUV420))
    goto cleanup;

  if (cip->entrypoint == GST_VAAPI_ENTRYPOINT_SLICE_ENCODE) {
    va_rate_control = from_GstVaapiRateControl (cip->rc_mode);
    if (va_rate_control == VA_RC_NONE)
      attribs[1].value = VA_RC_NONE;
    if ((attribs[1].value & va_rate_control) != va_rate_control) {
      GST_ERROR ("unsupported %s rate control",
          string_of_VARateControl (va_rate_control));
      goto cleanup;
    }
    attribs[1].value = va_rate_control;
  }

  GST_VAAPI_DISPLAY_LOCK (display);
  status = vaCreateConfig (GST_VAAPI_DISPLAY_VADISPLAY (display),
      context->va_profile, context->va_entrypoint, attribs, num_attribs,
      &context->va_config);
  GST_VAAPI_DISPLAY_UNLOCK (display);
  if (!vaapi_check_status (status, "vaCreateConfig()"))
    goto cleanup;

  GST_VAAPI_DISPLAY_LOCK (display);
  status = vaCreateContext (GST_VAAPI_DISPLAY_VADISPLAY (display),
      context->va_config, cip->width, cip->height, VA_PROGRESSIVE,
      (VASurfaceID *) surfaces->data, surfaces->len, &context_id);
  GST_VAAPI_DISPLAY_UNLOCK (display);
  if (!vaapi_check_status (status, "vaCreateContext()"))
    goto cleanup;

  GST_DEBUG ("context 0x%08x", context_id);
  GST_VAAPI_OBJECT_ID (context) = context_id;
  success = TRUE;

cleanup:
  if (surfaces)
    g_array_free (surfaces, TRUE);
  return success;
}

static inline void
gst_vaapi_context_init (GstVaapiContext * context,
    const GstVaapiContextInfo * cip)
{
  context->info = *cip;
  context->va_config = VA_INVALID_ID;
  gst_vaapi_context_overlay_init (context);
}

static void
gst_vaapi_context_finalize (GstVaapiContext * context)
{
  context_destroy (context);
  context_destroy_surfaces (context);
  gst_vaapi_context_overlay_finalize (context);
}

GST_VAAPI_OBJECT_DEFINE_CLASS (GstVaapiContext, gst_vaapi_context);

/**
 * gst_vaapi_context_new:
 * @display: a #GstVaapiDisplay
 * @cip: a pointer to the #GstVaapiContextInfo
 *
 * Creates a new #GstVaapiContext with the configuration specified by
 * @cip, thus including profile, entry-point, encoded size and maximum
 * number of reference frames reported by the bitstream.
 *
 * Return value: the newly allocated #GstVaapiContext object
 */
GstVaapiContext *
gst_vaapi_context_new (GstVaapiDisplay * display,
    const GstVaapiContextInfo * cip)
{
  GstVaapiContext *context;

  g_return_val_if_fail (cip->profile, NULL);
  g_return_val_if_fail (cip->entrypoint, NULL);
  g_return_val_if_fail (cip->width > 0, NULL);
  g_return_val_if_fail (cip->height > 0, NULL);

  context = gst_vaapi_object_new (gst_vaapi_context_class (), display);
  if (!context)
    return NULL;

  gst_vaapi_context_init (context, cip);
  if (!context_create (context))
    goto error;
  return context;

error:
  gst_vaapi_object_unref (context);
  return NULL;
}

/**
 * gst_vaapi_context_reset:
 * @context: a #GstVaapiContext
 * @new_cip: a pointer to the new #GstVaapiContextInfo details
 *
 * Resets @context to the configuration specified by @new_cip, thus
 * including profile, entry-point, encoded size and maximum number of
 * reference frames reported by the bitstream.
 *
 * Return value: %TRUE on success
 */
gboolean
gst_vaapi_context_reset (GstVaapiContext * context,
    const GstVaapiContextInfo * new_cip)
{
  GstVaapiContextInfo *const cip = &context->info;
  gboolean size_changed, config_changed;

  size_changed = cip->width != new_cip->width || cip->height != new_cip->height;
  if (size_changed) {
    cip->width = new_cip->width;
    cip->height = new_cip->height;
  }

  config_changed = cip->profile != new_cip->profile ||
      cip->entrypoint != new_cip->entrypoint;
  if (config_changed) {
    cip->profile = new_cip->profile;
    cip->entrypoint = new_cip->entrypoint;
  }

  switch (new_cip->entrypoint) {
    case GST_VAAPI_ENTRYPOINT_SLICE_ENCODE:
      if (cip->rc_mode != new_cip->rc_mode) {
        cip->rc_mode = new_cip->rc_mode;
        config_changed = TRUE;
      }
      break;
    default:
      break;
  }

  if (size_changed)
    context_destroy_surfaces (context);
  if (config_changed)
    context_destroy (context);

  if (size_changed && !context_create_surfaces (context))
    return FALSE;
  if (config_changed && !context_create (context))
    return FALSE;
  return TRUE;
}

/**
 * gst_vaapi_context_get_id:
 * @context: a #GstVaapiContext
 *
 * Returns the underlying VAContextID of the @context.
 *
 * Return value: the underlying VA context id
 */
GstVaapiID
gst_vaapi_context_get_id (GstVaapiContext * context)
{
  g_return_val_if_fail (context != NULL, VA_INVALID_ID);

  return GST_VAAPI_OBJECT_ID (context);
}

/**
 * gst_vaapi_context_get_surface_proxy:
 * @context: a #GstVaapiContext
 *
 * Acquires a free surface, wrapped into a #GstVaapiSurfaceProxy. The
 * returned surface will be automatically released when the proxy is
 * destroyed. So, it is enough to call gst_vaapi_surface_proxy_unref()
 * after usage.
 *
 * This function returns %NULL if there is no free surface available
 * in the pool. The surfaces are pre-allocated during context creation
 * though.
 *
 * Return value: a free surface, or %NULL if none is available
 */
GstVaapiSurfaceProxy *
gst_vaapi_context_get_surface_proxy (GstVaapiContext * context)
{
  g_return_val_if_fail (context != NULL, NULL);

  return
      gst_vaapi_surface_proxy_new_from_pool (GST_VAAPI_SURFACE_POOL
      (context->surfaces_pool));
}

/**
 * gst_vaapi_context_get_surface_count:
 * @context: a #GstVaapiContext
 *
 * Retrieves the number of free surfaces left in the pool.
 *
 * Return value: the number of free surfaces available in the pool
 */
guint
gst_vaapi_context_get_surface_count (GstVaapiContext * context)
{
  g_return_val_if_fail (context != NULL, 0);

  return gst_vaapi_video_pool_get_size (context->surfaces_pool);
}

/**
 * gst_vaapi_context_get_attribute:
 * @context: a #GstVaapiContext
 * @type: a VA config attribute type
 * @out_value_ptr: return location for the config attribute value
 *
 * Determines the value for the VA config attribute @type.
 *
 * Note: this function only returns success if the VA driver does
 * actually know about this config attribute type and that it returned
 * a valid value for it.
 *
 * Return value: %TRUE if the VA driver knows about the requested
 *   config attribute and returned a valid value, %FALSE otherwise
 */
gboolean
gst_vaapi_context_get_attribute (GstVaapiContext * context,
    VAConfigAttribType type, guint * out_value_ptr)
{
  VAConfigAttrib attrib;
  VAStatus status;

  g_return_val_if_fail (context != NULL, FALSE);

  GST_VAAPI_OBJECT_LOCK_DISPLAY (context);
  attrib.type = type;
  status = vaGetConfigAttributes (GST_VAAPI_OBJECT_VADISPLAY (context),
      context->va_profile, context->va_entrypoint, &attrib, 1);
  GST_VAAPI_OBJECT_UNLOCK_DISPLAY (context);
  if (!vaapi_check_status (status, "vaGetConfigAttributes()"))
    return FALSE;

  if (out_value_ptr)
    *out_value_ptr = attrib.value;
  return TRUE;
}
