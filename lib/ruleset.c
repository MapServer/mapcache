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
mapcache_rule* mapcache_rule_create(apr_pool_t *pool)
{
  mapcache_rule* rule = (mapcache_rule*)apr_pcalloc(pool, sizeof(mapcache_rule));
  rule->zoom_level = -1;
  rule->visible_extent = NULL;
  rule->visible_limits = NULL;
  rule->hidden_color = 0xffffff; //default = white
  rule->readonly = 0;
  return rule;
}

/*
 * clone a rule
 */
mapcache_rule* mapcache_rule_clone(apr_pool_t *pool, mapcache_rule *rule)
{
  mapcache_rule* clone = mapcache_rule_create(pool);

  clone->zoom_level = rule->zoom_level;
  clone->hidden_color = rule->hidden_color;
  clone->readonly = rule->readonly;

  if(rule->visible_extent) {
    clone->visible_extent = (mapcache_extent*)apr_pcalloc(pool, sizeof(mapcache_extent));
    *clone->visible_extent = *rule->visible_extent;
  }

  if(rule->visible_limits) {
    clone->visible_limits = (mapcache_extent_i*)apr_pcalloc(pool, sizeof(mapcache_extent_i));
    *clone->visible_limits = *rule->visible_limits;
  }
  
  return clone;
}

/*
 * get rule for zoom level, or NULL if none exist
 */
mapcache_rule* mapcache_rule_get(mapcache_ruleset *ruleset, int zoom_level)
{
  int i;
  mapcache_rule* rule;
  for(i = 0; i < ruleset->rules->nelts; i++) {
    if ((rule = APR_ARRAY_IDX(ruleset->rules, i, mapcache_rule*))->zoom_level == zoom_level) {
      return rule;
    }
  }
  return NULL;
}
