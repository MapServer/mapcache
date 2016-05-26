/******************************************************************************
 * $Id$
 *
 * Project:  MapServer
 * Purpose:  MapCache ruleset support file
 * Author:   Thomas Bonfort and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 1996-2011 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapcache.h"

/*
 * allocate and initialize a new ruleset
 */
mapcache_ruleset* mapcache_ruleset_create(apr_pool_t *pool)
{
  mapcache_ruleset* ruleset = (mapcache_ruleset*)apr_pcalloc(pool, sizeof(mapcache_ruleset));
  ruleset->rules = apr_array_make(pool,0,sizeof(mapcache_rule*));
  return ruleset;
}

/*
 * allocate and initialize a new rule
 */
mapcache_rule* mapcache_ruleset_rule_create(apr_pool_t *pool)
{
  mapcache_rule* rule = (mapcache_rule*)apr_pcalloc(pool, sizeof(mapcache_rule));
  rule->zoom_level = -1;
  rule->visible_extents = apr_array_make(pool,0,sizeof(mapcache_extent*));
  rule->visible_limits = apr_array_make(pool,0,sizeof(mapcache_extent_i*));
  rule->hidden_color = 0x00ffffff; // default is white with full transparency
  rule->hidden_tile = NULL;
  return rule;
}

/*
 * clone a rule
 */
mapcache_rule* mapcache_ruleset_rule_clone(apr_pool_t *pool, mapcache_rule *rule)
{
  mapcache_rule* clone = mapcache_ruleset_rule_create(pool);

  clone->zoom_level = rule->zoom_level;
  clone->hidden_color = rule->hidden_color;
  clone->hidden_tile = rule->hidden_tile; //no need to copy, just point to same buffer/tile.

  if(rule->visible_extents) {
    int i;
    for(i = 0; i < rule->visible_extents->nelts; i++) {
      mapcache_extent *extent_clone = (mapcache_extent*)apr_pcalloc(pool, sizeof(mapcache_extent));
      mapcache_extent *extent = APR_ARRAY_IDX(rule->visible_extents, i, mapcache_extent*);
      *extent_clone = *extent;
      APR_ARRAY_PUSH(clone->visible_extents, mapcache_extent*) = extent_clone;
    }
  }

  if(rule->visible_limits) {
    int i;
    for(i = 0; i < rule->visible_limits->nelts; i++) {
      mapcache_extent_i *extent_clone = (mapcache_extent_i*)apr_pcalloc(pool, sizeof(mapcache_extent_i));
      mapcache_extent_i *extent = APR_ARRAY_IDX(rule->visible_limits, i, mapcache_extent_i*);
      *extent_clone = *extent;
      APR_ARRAY_PUSH(clone->visible_limits, mapcache_extent_i*) = extent_clone;
    }
  }
  
  return clone;
}

/*
 * find rule for zoom level, or NULL if none exist
 */
mapcache_rule* mapcache_ruleset_rule_find(apr_array_header_t *rules, int zoom_level)
{
  int i;
  mapcache_rule* rule;

  if (!rules) {
    return NULL;
  }

  for(i = 0; i < rules->nelts; i++) {
    if ((rule = APR_ARRAY_IDX(rules, i, mapcache_rule*))->zoom_level == zoom_level) {
      return rule;
    }
  }
  return NULL;
}

/*
 * get rule at index, or NULL if index is out of bounds.
 */
mapcache_rule* mapcache_ruleset_rule_get(apr_array_header_t *rules, int idx)
{
  mapcache_rule *rule;

  if(!rules || idx < 0 || idx >= rules->nelts) {
    return NULL;
  }

  rule = APR_ARRAY_IDX(rules, idx, mapcache_rule*);
  return rule;
}

/*
 * check if tile is within visible limits
 */
int mapcache_ruleset_is_visible_tile(mapcache_rule* rule, mapcache_tile *tile) {
  int i;

  if(!rule || !rule->visible_limits || apr_is_empty_array(rule->visible_limits)) {
    return MAPCACHE_TRUE;
  }

  for(i = 0; i < rule->visible_limits->nelts; i++) {
    mapcache_extent_i *extent = APR_ARRAY_IDX(rule->visible_limits, i, mapcache_extent_i*);

    if(tile->x >= extent->minx && tile->y >= extent->miny && 
       tile->x <= extent->maxx && tile->y <= extent->maxy) {
      return MAPCACHE_TRUE;
    }
  }

  return MAPCACHE_FALSE;
}
