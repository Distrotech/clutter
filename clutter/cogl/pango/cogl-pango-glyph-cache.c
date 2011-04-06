/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 *
 * Copyright (C) 2008 OpenedHand
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
 * License along with this library. If not, see <http://www.gnu.org/licenses>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>

#include "cogl-pango-glyph-cache.h"
#include "cogl-pango-private.h"
#include "cogl/cogl-atlas.h"
#include "cogl/cogl-callback-list.h"

typedef struct _CoglPangoGlyphCacheKey     CoglPangoGlyphCacheKey;

struct _CoglPangoGlyphCache
{
  /* Hash table to quickly check whether a particular glyph in a
     particular font is already cached */
  GHashTable       *hash_table;

  /* List of CoglAtlases */
  GSList           *atlases;

  /* List of callbacks to invoke when an atlas is reorganized */
  CoglCallbackList  reorganize_callbacks;

  /* True if some of the glyphs are dirty. This is used as an
     optimization in _cogl_pango_glyph_cache_set_dirty_glyphs to avoid
     iterating the hash table if we know none of them are dirty */
  gboolean          has_dirty_glyphs;
};

struct _CoglPangoGlyphCacheKey
{
  PangoFont  *font;
  PangoGlyph  glyph;
};

static void
cogl_pango_glyph_cache_value_free (CoglPangoGlyphCacheValue *value)
{
  cogl_handle_unref (value->texture);
  g_slice_free (CoglPangoGlyphCacheValue, value);
}

static void
cogl_pango_glyph_cache_key_free (CoglPangoGlyphCacheKey *key)
{
  g_object_unref (key->font);
  g_slice_free (CoglPangoGlyphCacheKey, key);
}

static guint
cogl_pango_glyph_cache_hash_func (gconstpointer key)
{
  const CoglPangoGlyphCacheKey *cache_key
    = (const CoglPangoGlyphCacheKey *) key;

  /* Generate a number affected by both the font and the glyph
     number. We can safely directly compare the pointers because the
     key holds a reference to the font so it is not possible that a
     different font will have the same memory address */
  return GPOINTER_TO_UINT (cache_key->font) ^ cache_key->glyph;
}

static gboolean
cogl_pango_glyph_cache_equal_func (gconstpointer a,
				      gconstpointer b)
{
  const CoglPangoGlyphCacheKey *key_a
    = (const CoglPangoGlyphCacheKey *) a;
  const CoglPangoGlyphCacheKey *key_b
    = (const CoglPangoGlyphCacheKey *) b;

  /* We can safely directly compare the pointers for the fonts because
     the key holds a reference to the font so it is not possible that
     a different font will have the same memory address */
  return key_a->font == key_b->font
    && key_a->glyph == key_b->glyph;
}

CoglPangoGlyphCache *
cogl_pango_glyph_cache_new (void)
{
  CoglPangoGlyphCache *cache;

  cache = g_malloc (sizeof (CoglPangoGlyphCache));

  cache->hash_table = g_hash_table_new_full
    (cogl_pango_glyph_cache_hash_func,
     cogl_pango_glyph_cache_equal_func,
     (GDestroyNotify) cogl_pango_glyph_cache_key_free,
     (GDestroyNotify) cogl_pango_glyph_cache_value_free);

  cache->atlases = NULL;
  _cogl_callback_list_init (&cache->reorganize_callbacks);

  cache->has_dirty_glyphs = FALSE;

  return cache;
}

void
cogl_pango_glyph_cache_clear (CoglPangoGlyphCache *cache)
{
  g_slist_foreach (cache->atlases, (GFunc) cogl_object_unref, NULL);
  g_slist_free (cache->atlases);
  cache->atlases = NULL;
  cache->has_dirty_glyphs = FALSE;

  g_hash_table_remove_all (cache->hash_table);
}

void
cogl_pango_glyph_cache_free (CoglPangoGlyphCache *cache)
{
  cogl_pango_glyph_cache_clear (cache);

  g_hash_table_unref (cache->hash_table);

  _cogl_callback_list_destroy (&cache->reorganize_callbacks);

  g_free (cache);
}

static void
cogl_pango_glyph_cache_update_position_cb (void *user_data,
                                           CoglHandle new_texture,
                                           const CoglRectangleMapEntry *rect)
{
  CoglPangoGlyphCacheValue *value = user_data;
  float tex_width, tex_height;

  if (value->texture)
    cogl_handle_unref (value->texture);
  value->texture = cogl_handle_ref (new_texture);

  tex_width = cogl_texture_get_width (new_texture);
  tex_height = cogl_texture_get_height (new_texture);

  value->tx1 = rect->x / tex_width;
  value->ty1 = rect->y / tex_height;
  value->tx2 = (rect->x + value->draw_width) / tex_width;
  value->ty2 = (rect->y + value->draw_height) / tex_height;

  value->tx_pixel = rect->x;
  value->ty_pixel = rect->y;

  /* The glyph has changed position so it will need to be redrawn */
  value->dirty = TRUE;
}

static void
cogl_pango_glyph_cache_reorganize_cb (void *user_data)
{
  CoglPangoGlyphCache *cache = user_data;

  _cogl_callback_list_invoke (&cache->reorganize_callbacks);
}

CoglPangoGlyphCacheValue *
cogl_pango_glyph_cache_lookup (CoglPangoGlyphCache *cache,
                               gboolean             create,
                               PangoFont           *font,
                               PangoGlyph           glyph)
{
  CoglPangoGlyphCacheKey lookup_key;
  CoglPangoGlyphCacheValue *value;

  lookup_key.font = font;
  lookup_key.glyph = glyph;

  value = g_hash_table_lookup (cache->hash_table, &lookup_key);

  if (create && value == NULL)
    {
      CoglPangoGlyphCacheKey *key;
      PangoRectangle ink_rect;
      CoglAtlas *atlas = NULL;
      GSList *l;

      pango_font_get_glyph_extents (font, glyph, &ink_rect, NULL);
      pango_extents_to_pixels (&ink_rect, NULL);

      value = g_slice_new (CoglPangoGlyphCacheValue);
      value->texture = COGL_INVALID_HANDLE;
      value->draw_x = ink_rect.x;
      value->draw_y = ink_rect.y;
      value->draw_width = ink_rect.width;
      value->draw_height = ink_rect.height;
      value->dirty = TRUE;

      /* Look for an atlas that can reserve the space */
      for (l = cache->atlases; l; l = l->next)
        if (_cogl_atlas_reserve_space (l->data,
                                       ink_rect.width + 1, ink_rect.height + 1,
                                       value))
          {
            atlas = l->data;
            break;
          }

      /* If we couldn't find one then start a new atlas */
      if (atlas == NULL)
        {
          atlas = _cogl_atlas_new (COGL_PIXEL_FORMAT_A_8,
                                   COGL_ATLAS_CLEAR_TEXTURE |
                                   COGL_ATLAS_DISABLE_MIGRATION,
                                   cogl_pango_glyph_cache_update_position_cb);
          COGL_NOTE (ATLAS, "Created new atlas for glyphs: %p", atlas);
          /* If we still can't reserve space then something has gone
             seriously wrong so we'll just give up */
          if (!_cogl_atlas_reserve_space (atlas,
                                          ink_rect.width + 1,
                                          ink_rect.height + 1, value))
            {
              cogl_object_unref (atlas);
              cogl_pango_glyph_cache_value_free (value);
              return NULL;
            }

          _cogl_atlas_add_reorganize_callback
            (atlas, cogl_pango_glyph_cache_reorganize_cb, NULL, cache);

          cache->atlases = g_slist_prepend (cache->atlases, atlas);
        }

      key = g_slice_new (CoglPangoGlyphCacheKey);
      key->font = g_object_ref (font);
      key->glyph = glyph;

      g_hash_table_insert (cache->hash_table, key, value);

      cache->has_dirty_glyphs = TRUE;
    }

  return value;
}

static void
_cogl_pango_glyph_cache_set_dirty_glyphs_cb (gpointer key_ptr,
                                             gpointer value_ptr,
                                             gpointer user_data)
{
  CoglPangoGlyphCacheKey *key = key_ptr;
  CoglPangoGlyphCacheValue *value = value_ptr;
  CoglPangoGlyphCacheDirtyFunc func = user_data;

  if (value->dirty)
    {
      func (key->font, key->glyph, value);

      value->dirty = FALSE;
    }
}

void
_cogl_pango_glyph_cache_set_dirty_glyphs (CoglPangoGlyphCache *cache,
                                          CoglPangoGlyphCacheDirtyFunc func)
{
  /* If we know that there are no dirty glyphs then we can shortcut
     out early */
  if (!cache->has_dirty_glyphs)
    return;

  g_hash_table_foreach (cache->hash_table,
                        _cogl_pango_glyph_cache_set_dirty_glyphs_cb,
                        func);

  cache->has_dirty_glyphs = FALSE;
}

void
_cogl_pango_glyph_cache_add_reorganize_callback (CoglPangoGlyphCache *cache,
                                                 CoglCallbackListFunc func,
                                                 void *user_data)
{
  _cogl_callback_list_add (&cache->reorganize_callbacks, func, user_data);
}

void
_cogl_pango_glyph_cache_remove_reorganize_callback (CoglPangoGlyphCache *cache,
                                                    CoglCallbackListFunc func,
                                                    void *user_data)
{
  _cogl_callback_list_remove (&cache->reorganize_callbacks, func, user_data);
}
