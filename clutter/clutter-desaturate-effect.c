/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2010  Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author:
 *   Emmanuele Bassi <ebassi@linux.intel.com>
 */

/**
 * SECTION:clutter-desaturate-effect
 * @short_description: A desaturation effect
 * @see_also: #ClutterEffect, #ClutterOffscreenEffect
 *
 * #ClutterDesaturateEffect is a sub-class of #ClutterEffect that
 * desaturates the color of an actor and its contents. The strenght
 * of the desaturation effect is controllable and animatable through
 * the #ClutterDesaturateEffect:factor property.
 *
 * #ClutterDesaturateEffect is available since Clutter 1.4
 */

#define CLUTTER_DESATURATE_EFFECT_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), CLUTTER_TYPE_DESATURATE_EFFECT, ClutterDesaturateEffectClass))
#define CLUTTER_IS_DESATURATE_EFFECT_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), CLUTTER_TYPE_DESATURATE_EFFECT))
#define CLUTTER_DESATURATE_EFFECT_GET_CLASS(obj)      (G_TYPE_INSTANCE_GET_CLASS ((obj), CLUTTER_TYPE_DESATURATE_EFFECT, ClutterDesaturateEffectClass))

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>

#include "clutter-desaturate-effect.h"

#include "cogl/cogl.h"

#include "clutter-debug.h"
#include "clutter-enum-types.h"
#include "clutter-offscreen-effect.h"
#include "clutter-private.h"

struct _ClutterDesaturateEffect
{
  ClutterOffscreenEffect parent_instance;

  /* a back pointer to our actor, so that we can query it */
  ClutterActor *actor;

  /* the desaturation factor, also known as "strength" */
  gdouble factor;

  gint factor_uniform;

  CoglPipeline *pipeline;
};

struct _ClutterDesaturateEffectClass
{
  ClutterOffscreenEffectClass parent_class;
};

static CoglPipeline *base_pipeline = NULL;

/* the magic gray vec3 has been taken from the NTSC conversion weights
 * as defined by:
 *
 *   "OpenGL Superbible, 4th edition"
 *   -- Richard S. Wright Jr, Benjamin Lipchak, Nicholas Haemel
 *   Addison-Wesley
 */
static const gchar *desaturate_glsl_declarations =
  "uniform float factor;\n"
  "\n"
  "vec3 desaturate (const vec3 color, const float desaturation)\n"
  "{\n"
  "  const vec3 gray_conv = vec3 (0.299, 0.587, 0.114);\n"
  "  vec3 gray = vec3 (dot (gray_conv, color));\n"
  "  return vec3 (mix (color.rgb, gray, desaturation));\n"
  "}\n";

static const gchar *desaturate_glsl_source =
  "  cogl_color_out.rgb = desaturate (cogl_color_out.rgb, factor);\n";

enum
{
  PROP_0,

  PROP_FACTOR,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

G_DEFINE_TYPE (ClutterDesaturateEffect,
               clutter_desaturate_effect,
               CLUTTER_TYPE_OFFSCREEN_EFFECT);

static gboolean
clutter_desaturate_effect_pre_paint (ClutterEffect *effect)
{
  ClutterDesaturateEffect *self = CLUTTER_DESATURATE_EFFECT (effect);
  ClutterEffectClass *parent_class;

  if (!clutter_actor_meta_get_enabled (CLUTTER_ACTOR_META (effect)))
    return FALSE;

  self->actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (effect));
  if (self->actor == NULL)
    return FALSE;

  if (!clutter_feature_available (CLUTTER_FEATURE_SHADERS_GLSL))
    {
      /* if we don't have support for GLSL shaders then we
       * forcibly disable the ActorMeta
       */
      g_warning ("Unable to use the ShaderEffect: the graphics hardware "
                 "or the current GL driver does not implement support "
                 "for the GLSL shading language.");
      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (effect), FALSE);
      return FALSE;
    }

  parent_class = CLUTTER_EFFECT_CLASS (clutter_desaturate_effect_parent_class);
  return parent_class->pre_paint (effect);
}

static void
clutter_desaturate_effect_paint_target (ClutterOffscreenEffect *effect)
{
  ClutterDesaturateEffect *self = CLUTTER_DESATURATE_EFFECT (effect);
  ClutterActor *actor;
  CoglHandle texture;
  guint8 paint_opacity;

  if (self->factor_uniform > -1)
    cogl_pipeline_set_uniform_1f (self->pipeline,
                                  self->factor_uniform,
                                  self->factor);

  texture = clutter_offscreen_effect_get_texture (effect);
  cogl_pipeline_set_layer_texture (self->pipeline, 0, texture);

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (effect));
  paint_opacity = clutter_actor_get_paint_opacity (actor);

  cogl_pipeline_set_color4ub (self->pipeline,
                              paint_opacity,
                              paint_opacity,
                              paint_opacity,
                              paint_opacity);
  cogl_push_source (self->pipeline);

  cogl_rectangle (0, 0,
                  cogl_texture_get_width (texture),
                  cogl_texture_get_height (texture));

  cogl_pop_source ();
}

static void
clutter_desaturate_effect_dispose (GObject *gobject)
{
  ClutterDesaturateEffect *self = CLUTTER_DESATURATE_EFFECT (gobject);

  if (self->pipeline != NULL)
    {
      cogl_object_unref (self->pipeline);
      self->pipeline = NULL;
    }

  G_OBJECT_CLASS (clutter_desaturate_effect_parent_class)->dispose (gobject);
}

static void
clutter_desaturate_effect_set_property (GObject      *gobject,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ClutterDesaturateEffect *effect = CLUTTER_DESATURATE_EFFECT (gobject);

  switch (prop_id)
    {
    case PROP_FACTOR:
      clutter_desaturate_effect_set_factor (effect,
                                            g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_desaturate_effect_get_property (GObject    *gobject,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ClutterDesaturateEffect *effect = CLUTTER_DESATURATE_EFFECT (gobject);

  switch (prop_id)
    {
    case PROP_FACTOR:
      g_value_set_double (value, effect->factor);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_desaturate_effect_class_init (ClutterDesaturateEffectClass *klass)
{
  ClutterEffectClass *effect_class = CLUTTER_EFFECT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterOffscreenEffectClass *offscreen_class;

  offscreen_class = CLUTTER_OFFSCREEN_EFFECT_CLASS (klass);
  offscreen_class->paint_target = clutter_desaturate_effect_paint_target;

  effect_class->pre_paint = clutter_desaturate_effect_pre_paint;

  /**
   * ClutterDesaturateEffect:factor:
   *
   * The desaturation factor, between 0.0 (no desaturation) and 1.0 (full
   * desaturation).
   *
   * Since: 1.4
   */
  obj_props[PROP_FACTOR] =
    g_param_spec_double ("factor",
                         P_("Factor"),
                         P_("The desaturation factor"),
                         0.0, 1.0,
                         1.0,
                         CLUTTER_PARAM_READWRITE);

  gobject_class->dispose = clutter_desaturate_effect_dispose;
  gobject_class->set_property = clutter_desaturate_effect_set_property;
  gobject_class->get_property = clutter_desaturate_effect_get_property;

  g_object_class_install_properties (gobject_class,
                                     PROP_LAST,
                                     obj_props);
}

static void
clutter_desaturate_effect_init (ClutterDesaturateEffect *self)
{
  if (base_pipeline == NULL)
    {
      CoglHandle dummy_texture;

      base_pipeline = cogl_pipeline_new ();

      if (clutter_feature_available (CLUTTER_FEATURE_SHADERS_GLSL))
        {
          CoglSnippet *snippet;

          snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                                      desaturate_glsl_declarations,
                                      desaturate_glsl_source);
          cogl_pipeline_add_snippet (base_pipeline, snippet);
          cogl_object_unref (snippet);
        }

      dummy_texture = cogl_texture_new_with_size (1, 1,
                                                  COGL_TEXTURE_NONE,
                                                  COGL_PIXEL_FORMAT_RGB_888);
      cogl_pipeline_set_layer_texture (base_pipeline, 0, dummy_texture);
      cogl_handle_unref (dummy_texture);
    }

  self->pipeline = cogl_pipeline_copy (base_pipeline);

  self->factor_uniform =
    cogl_pipeline_get_uniform_location (self->pipeline, "factor");

  self->factor = 1.0;
}

/**
 * clutter_desaturate_effect_new:
 * @factor: the desaturation factor, between 0.0 and 1.0
 *
 * Creates a new #ClutterDesaturateEffect to be used with
 * clutter_actor_add_effect()
 *
 * Return value: the newly created #ClutterDesaturateEffect or %NULL
 *
 * Since: 1.4
 */
ClutterEffect *
clutter_desaturate_effect_new (gdouble factor)
{
  g_return_val_if_fail (factor >= 0.0 && factor <= 1.0, NULL);

  return g_object_new (CLUTTER_TYPE_DESATURATE_EFFECT,
                       "factor", factor,
                       NULL);
}

/**
 * clutter_desaturate_effect_set_factor:
 * @effect: a #ClutterDesaturateEffect
 * @factor: the desaturation factor, between 0.0 and 1.0
 *
 * Sets the desaturation factor for @effect, with 0.0 being "do not desaturate"
 * and 1.0 being "fully desaturate"
 *
 * Since: 1.4
 */
void
clutter_desaturate_effect_set_factor (ClutterDesaturateEffect *effect,
                                      gdouble                  factor)
{
  g_return_if_fail (CLUTTER_IS_DESATURATE_EFFECT (effect));
  g_return_if_fail (factor >= 0.0 && factor <= 1.0);

  if (fabsf (effect->factor - factor) >= 0.00001)
    {
      effect->factor = factor;

      if (effect->actor != NULL)
        clutter_actor_queue_redraw (effect->actor);

      g_object_notify_by_pspec (G_OBJECT (effect), obj_props[PROP_FACTOR]);
    }
}

/**
 * clutter_desaturate_effect_get_factor:
 * @effect: a #ClutterDesaturateEffect
 *
 * Retrieves the desaturation factor of @effect
 *
 * Return value: the desaturation factor
 *
 * Since: 1.4
 */
gdouble
clutter_desaturate_effect_get_factor (ClutterDesaturateEffect *effect)
{
  g_return_val_if_fail (CLUTTER_IS_DESATURATE_EFFECT (effect), 0.0);

  return effect->factor;
}
