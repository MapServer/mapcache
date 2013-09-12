/******************************************************************************
 * $Id: mapcache_seed.c 13201 2012-03-05 13:50:45Z tbonfort $
 *
 * Project:  MapServer
 * Purpose:  MapCache utility program for seeding and pruning caches
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

#include "mapcache-util-config.h"
#include "mapcache.h"
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <apr_getopt.h>
#include <signal.h>

#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#define USE_FORK
#include <sys/time.h>
#endif

#include <apr_time.h>
#include <apr_strings.h>

#ifdef USE_FORK
int msqid;
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#endif

#include <apr_queue.h>
apr_queue_t *work_queue;

#if defined(USE_OGR) && defined(USE_GEOS)
#define USE_CLIPPERS
#endif

#ifdef USE_CLIPPERS
#include "ogr_api.h"
#include "geos_c.h"
int nClippers = 0;
const GEOSPreparedGeometry **clippers=NULL;
#endif

mapcache_tileset *tileset;
mapcache_tileset *tileset_transfer;
mapcache_cfg *cfg;
mapcache_context ctx;
apr_table_t *dimensions=NULL;
int minzoom=-1;
int maxzoom=-1;
mapcache_grid_link *grid_link;
int nthreads=0;
int nprocesses=0;
int quiet = 0;
int verbose = 0;
int force = 0;
int sig_int_received = 0;
int error_detected = 0;

apr_time_t age_limit = 0;
int seededtilestot=0, seededtiles=0, queuedtilestot=0;
struct mctimeval lastlogtime,starttime;

typedef enum {
  MAPCACHE_CMD_SEED,
  MAPCACHE_CMD_STOP,
  MAPCACHE_CMD_DELETE,
  MAPCACHE_CMD_SKIP,
  MAPCACHE_CMD_TRANSFER
} cmd;

typedef enum {
  MAPCACHE_ITERATION_UNSET,
  MAPCACHE_ITERATION_DEPTH_FIRST,
  MAPCACHE_ITERATION_LEVEL_FIRST
} mapcache_iteration_mode;

mapcache_iteration_mode iteration_mode = MAPCACHE_ITERATION_UNSET;

struct seed_cmd {
  cmd command;
  int x;
  int y;
  int z;
};

#ifdef USE_FORK
struct msg_cmd {
  long mtype;
  struct seed_cmd cmd;
};
#endif

cmd mode = MAPCACHE_CMD_SEED; /* the mode the utility will be running in: either seed or delete */

int push_queue(struct seed_cmd cmd)
{
#ifdef USE_FORK
  if(nprocesses > 1) {
    struct msg_cmd mcmd;
    mcmd.mtype = 1;
    mcmd.cmd = cmd;
    if (msgsnd(msqid, &mcmd, sizeof(struct seed_cmd), 0) == -1) {
      printf("failed to push tile %d %d %d\n",cmd.z,cmd.y,cmd.x);
      return APR_EGENERAL;
    }
    return APR_SUCCESS;
  }
#endif
  struct seed_cmd *pcmd = calloc(1,sizeof(struct seed_cmd));
  *pcmd = cmd;
  return apr_queue_push(work_queue,pcmd);
}

int pop_queue(struct seed_cmd *cmd)
{
  int ret;
  struct seed_cmd *pcmd;

#ifdef USE_FORK
  if(nprocesses > 1) {
    struct msg_cmd mcmd;
    if (msgrcv(msqid, &mcmd, sizeof(struct seed_cmd), 1, 0) == -1) {
      printf("failed to pop tile\n");
      return APR_EGENERAL;
    }
    *cmd = mcmd.cmd;
    return APR_SUCCESS;
  }
#endif
  
  ret = apr_queue_pop(work_queue, (void**)&pcmd);
  if(ret == APR_SUCCESS) {
    *cmd = *pcmd;
    free(pcmd);
  }
  return ret;
}

int trypop_queue(struct seed_cmd *cmd)
{
  int ret;
  struct seed_cmd *pcmd;

#ifdef USE_FORK
  if(nprocesses>1) {
    struct msg_cmd mcmd;
    ret = msgrcv(msqid, &mcmd, sizeof(struct seed_cmd), 1, IPC_NOWAIT);
    if(errno == ENOMSG) return APR_EAGAIN;
    if(ret>0) {
      *cmd = mcmd.cmd;
      return APR_SUCCESS;
    } else {
      printf("failed to trypop tile\n");
      return APR_EGENERAL;
    }
  }
#endif
  ret = apr_queue_trypop(work_queue,(void**)&pcmd);
  if(ret == APR_SUCCESS) {
    *cmd = *pcmd;
    free(pcmd);
  }
  return ret;
}

static const apr_getopt_option_t seed_options[] = {
  /* long-option, short-option, has-arg flag, description */
  { "config", 'c', TRUE, "configuration file (/path/to/mapcache.xml)"},
#ifdef USE_CLIPPERS
  { "ogr-datasource", 'd', TRUE, "ogr datasource to get features from"},
#endif
  { "dimension", 'D', TRUE, "set the value of a dimension (format DIMENSIONNAME=VALUE). Can be used multiple times for multiple dimensions" },
  { "extent", 'e', TRUE, "extent to seed, format: minx,miny,maxx,maxy" },
  { "force", 'f', FALSE, "force tile recreation even if it already exists" },
  { "grid", 'g', TRUE, "grid to seed" },
  { "help", 'h', FALSE, "show help" },
  { "iteration-mode", 'i', TRUE, "either \"drill-down\" or \"level-by-level\". Default is to use drill-down for g, WGS84 and GoogleMapsCompatible grids, and level-by-level for others. Use this flag to override." },
#ifdef USE_CLIPPERS
  { "ogr-layer", 'l', TRUE, "layer inside datasource"},
#endif
  { "mode", 'm', TRUE, "mode: seed (default), delete or transfer" },
  { "metasize", 'M', TRUE, "override metatile size while seeding, eg 8,8" },
  { "nthreads", 'n', TRUE, "number of parallel threads to use (incompatible with -p/--nprocesses)" },
  { "older", 'o', TRUE, "reseed tiles older than supplied date (format: year/month/day hour:minute, eg: 2011/01/31 20:45" },
  { "nprocesses", 'p', TRUE, "number of parallel processes to use (incompatible with -n/--nthreads)" },
  { "quiet", 'q', FALSE, "don't show progress info" },
#ifdef USE_CLIPPERS
  { "ogr-sql", 's', TRUE, "sql to filter inside layer"},
#endif
  { "tileset", 't', TRUE, "tileset to seed" },
  { "verbose", 'v', FALSE, "show debug log messages" },
#ifdef USE_CLIPPERS
  { "ogr-where", 'w', TRUE, "filter to apply on layer features"},
#endif
  { "transfer", 'x', TRUE, "tileset to transfer" },
  { "zoom", 'z', TRUE, "min and max zoomlevels to seed, separated by a comma. eg 0,6" },
  { NULL, 0, 0, NULL }
};

void handle_sig_int(int signal)
{
  if(!sig_int_received) {
    fprintf(stderr,"SIGINT received, waiting for threads to finish\n");
    fprintf(stderr,"press ctrl-C again to force terminate, you might end up with locked tiles\n");
    sig_int_received = 1;
  } else {
    exit(signal);
  }
}

void seed_log(mapcache_context *ctx, mapcache_log_level level, char *msg, ...)
{
  if(verbose) {
    va_list args;
    va_start(args,msg);
    vfprintf(stderr,msg,args);
    va_end(args);
    printf("\n");
  }
}


void mapcache_context_seeding_log(mapcache_context *ctx, mapcache_log_level level, char *msg, ...)
{
  va_list args;
  va_start(args,msg);
  vfprintf(stderr,msg,args);
  va_end(args);
  printf("\n");
}

#ifdef USE_CLIPPERS
int ogr_features_intersect_tile(mapcache_context *ctx, mapcache_tile *tile)
{
  mapcache_metatile *mt = mapcache_tileset_metatile_get(ctx,tile);
  GEOSCoordSequence *mtbboxls = GEOSCoordSeq_create(5,2);
  GEOSCoordSeq_setX(mtbboxls,0,mt->map.extent.minx);
  GEOSCoordSeq_setY(mtbboxls,0,mt->map.extent.miny);
  GEOSCoordSeq_setX(mtbboxls,1,mt->map.extent.maxx);
  GEOSCoordSeq_setY(mtbboxls,1,mt->map.extent.miny);
  GEOSCoordSeq_setX(mtbboxls,2,mt->map.extent.maxx);
  GEOSCoordSeq_setY(mtbboxls,2,mt->map.extent.maxy);
  GEOSCoordSeq_setX(mtbboxls,3,mt->map.extent.minx);
  GEOSCoordSeq_setY(mtbboxls,3,mt->map.extent.maxy);
  GEOSCoordSeq_setX(mtbboxls,4,mt->map.extent.minx);
  GEOSCoordSeq_setY(mtbboxls,4,mt->map.extent.miny);
  GEOSGeometry *mtbbox = GEOSGeom_createLinearRing(mtbboxls);
  GEOSGeometry *mtbboxg = GEOSGeom_createPolygon(mtbbox,NULL,0);
  int i;
  int intersects = 0;
  for(i=0; i<nClippers; i++) {
    const GEOSPreparedGeometry *clipper = clippers[i];
    if(GEOSPreparedIntersects(clipper,mtbboxg)) {
      intersects = 1;
      break;
    }
  }
  GEOSGeom_destroy(mtbboxg);
  return intersects;
}

#endif

int lastmsglen = 0;
void progresslog(int x, int y, int z)
{
  char msg[1024];
  int nworkers;
  if(quiet) return;
  nworkers = nthreads;
  if(nprocesses >= 1) nworkers = nprocesses;

  sprintf(msg,"seeding tile %d %d %d",x,y,z);
  if(lastmsglen) {
    char erasestring[1024];
    int len = MAPCACHE_MIN(1023,lastmsglen);
    memset(erasestring,' ',len);
    erasestring[len+1]='\0';
    sprintf(erasestring,"\r%%%ds\r",lastmsglen);
    printf(erasestring," ");
  }
  lastmsglen = strlen(msg);
  printf("%s",msg);
  fflush(NULL);
  return;

  if(queuedtilestot>nworkers) {
    struct mctimeval now_t;
    float duration;
    float totalduration;
    seededtilestot = queuedtilestot - nworkers;

    mapcache_gettimeofday(&now_t,NULL);
    duration = ((now_t.tv_sec-lastlogtime.tv_sec)*1000000+(now_t.tv_usec-lastlogtime.tv_usec))/1000000.0;
    totalduration = ((now_t.tv_sec-starttime.tv_sec)*1000000+(now_t.tv_usec-starttime.tv_usec))/1000000.0;
    if(duration>=5) {
      int Nx, Ny, Ntot, Ncur, ntilessincelast;
      Nx = (grid_link->grid_limits[z].maxx-grid_link->grid_limits[z].minx)/tileset->metasize_x;
      Ny = (grid_link->grid_limits[z].maxy-grid_link->grid_limits[z].miny)/tileset->metasize_y;
      Ntot = Nx*Ny;
      Ncur = ((y-grid_link->grid_limits[z].miny)/tileset->metasize_y ) * Nx +
          (x-grid_link->grid_limits[z].minx+1)/tileset->metasize_x;
      ntilessincelast = seededtilestot-seededtiles;
      sprintf(msg,"seeding level %d [%d/%d]: %f metatiles/sec (avg since start: %f)",z,Ncur,Ntot,ntilessincelast/duration,
              seededtilestot/totalduration);
      lastlogtime=now_t;
      seededtiles=seededtilestot;
    } else {
      return;
    }
  } else {
    sprintf(msg,"seeding level %d",z);

  }
  if(lastmsglen) {
    char erasestring[1024];
    int len = MAPCACHE_MIN(1023,lastmsglen);
    memset(erasestring,' ',len);
    erasestring[len+1]='\0';
    sprintf(erasestring,"\r%%%ds\r",lastmsglen);
    printf(erasestring," ");
  }
  lastmsglen = strlen(msg);
  printf("%s",msg);
  fflush(NULL);
}

cmd examine_tile(mapcache_context *ctx, mapcache_tile *tile)
{
  int action = MAPCACHE_CMD_SKIP;
  int intersects = -1;
  int tile_exists = force?0:tileset->cache->tile_exists(ctx,tile);

  /* if the tile exists and a time limit was specified, check the tile modification date */
  if(tile_exists) {
    if(age_limit) {
      if(tileset->cache->tile_get(ctx,tile) == MAPCACHE_SUCCESS) {
        if(tile->mtime && tile->mtime<age_limit) {
          /* the tile modification time is older than the specified limit */
#ifdef USE_CLIPPERS
          /* check we are in the requested features before deleting the tile */
          if(nClippers > 0) {
            intersects = ogr_features_intersect_tile(ctx,tile);
          }
#endif
          if(intersects != 0) {
            /* the tile intersects the ogr features, or there was no clipping asked for: seed it */
            if(mode == MAPCACHE_CMD_SEED || mode == MAPCACHE_CMD_TRANSFER) {
              mapcache_tileset_tile_delete(ctx,tile,MAPCACHE_TRUE);
              /* if we are in mode transfer, delete it from the dst tileset */
              if (mode == MAPCACHE_CMD_TRANSFER) {
                tile->tileset = tileset_transfer;
                if (tileset_transfer->cache->tile_exists(ctx,tile)) {
                  mapcache_tileset_tile_delete(ctx,tile,MAPCACHE_TRUE);
                }
                tile->tileset = tileset;
              }
              action = mode;
            } else { //if(action == MAPCACHE_CMD_DELETE)
              action = MAPCACHE_CMD_DELETE;
            }
          } else {
            /* the tile does not intersect the ogr features, and already exists, do nothing */
            action = MAPCACHE_CMD_SKIP;
          }
        }
      } else {
        //BUG: tile_exists returned true, but tile_get returned a failure. not sure what to do.
        action = MAPCACHE_CMD_SKIP;
      }
    } else {
      if(mode == MAPCACHE_CMD_DELETE) {
        //the tile exists and we are in delete mode: delete it
        action = MAPCACHE_CMD_DELETE;
      } else if (mode == MAPCACHE_CMD_TRANSFER) {
        /* the tile exists in the source tileset,
           check if the tile exists in the destination cache */
        tile->tileset = tileset_transfer;
        if (tileset_transfer->cache->tile_exists(ctx,tile)) {
          action = MAPCACHE_CMD_SKIP;
        } else {
          action = MAPCACHE_CMD_TRANSFER;
        }
        tile->tileset = tileset;
      } else {
        // the tile exists and we are in seed mode, skip to next one
        action = MAPCACHE_CMD_SKIP;
      }
    }
  } else {
    // the tile does not exist
    if(mode == MAPCACHE_CMD_SEED || mode == MAPCACHE_CMD_TRANSFER) {
#ifdef USE_CLIPPERS
      /* check we are in the requested features before deleting the tile */
      if(nClippers > 0) {
        if(ogr_features_intersect_tile(ctx,tile)) {
          action = mode;
        } else {
          action = MAPCACHE_CMD_SKIP;
        }
      } else {
        action = mode;
      }
#else
      action = mode;
#endif
    } else {
      action = MAPCACHE_CMD_SKIP;
    }
  }

  return action;
}

void cmd_recurse(mapcache_context *cmd_ctx, mapcache_tile *tile)
{
  cmd action;
  int curx, cury, curz;
  int blchildx,trchildx,blchildy,trchildy;
  int minchildx,maxchildx,minchildy,maxchildy;
  mapcache_extent bboxbl,bboxtr;
  double epsilon;

  apr_pool_clear(cmd_ctx->pool);
  if(sig_int_received || error_detected) { //stop if we were asked to stop by hitting ctrl-c
    //remove all items from the queue
    struct seed_cmd entry;
    while (trypop_queue(&entry)!=APR_EAGAIN) {
      queuedtilestot--;
    }
    return;
  }

  action = examine_tile(cmd_ctx, tile);

  if(action == MAPCACHE_CMD_SEED || action == MAPCACHE_CMD_DELETE || action == MAPCACHE_CMD_TRANSFER) {
    //current x,y,z needs seeding, add it to the queue
    struct seed_cmd cmd;
    cmd.x = tile->x;
    cmd.y = tile->y;
    cmd.z = tile->z;
    cmd.command = action;
    push_queue(cmd);
    queuedtilestot++;
    progresslog(tile->x,tile->y,tile->z);
  }

  //recurse into our 4 child metatiles

  curx = tile->x;
  cury = tile->y;
  curz = tile->z;
  tile->z += 1;
  if(tile->z > maxzoom) {
    tile->z -= 1;
    return;
  }

  /*
   * compute the x,y limits of the next zoom level that intersect the
   * current metatile
   */


  mapcache_grid_get_extent(cmd_ctx, grid_link->grid,
                           curx, cury, curz, &bboxbl);
  mapcache_grid_get_extent(cmd_ctx, grid_link->grid,
                           curx+tileset->metasize_x-1, cury+tileset->metasize_y-1, curz, &bboxtr);
  epsilon = (bboxbl.maxx-bboxbl.minx)*0.01;
  mapcache_grid_get_xy(cmd_ctx,grid_link->grid,
                       bboxbl.minx + epsilon,
                       bboxbl.miny + epsilon,
                       tile->z,&blchildx,&blchildy);
  mapcache_grid_get_xy(cmd_ctx,grid_link->grid,
                       bboxtr.maxx - epsilon,
                       bboxtr.maxy - epsilon,
                       tile->z,&trchildx,&trchildy);

  minchildx = (MAPCACHE_MIN(blchildx,trchildx) / tileset->metasize_x)*tileset->metasize_x;
  minchildy = (MAPCACHE_MIN(blchildy,trchildy) / tileset->metasize_y)*tileset->metasize_y;
  maxchildx = (MAPCACHE_MAX(blchildx,trchildx) / tileset->metasize_x + 1)*tileset->metasize_x;
  maxchildy = (MAPCACHE_MAX(blchildy,trchildy) / tileset->metasize_y + 1)*tileset->metasize_y;

  for(tile->x = minchildx; tile->x < maxchildx; tile->x +=  tileset->metasize_x) {
    if(tile->x >= grid_link->grid_limits[tile->z].minx && tile->x < grid_link->grid_limits[tile->z].maxx) {
      for(tile->y = minchildy; tile->y < maxchildy; tile->y += tileset->metasize_y) {
        if(tile->y >= grid_link->grid_limits[tile->z].miny && tile->y < grid_link->grid_limits[tile->z].maxy) {
          cmd_recurse(cmd_ctx,tile);
        }
      }
    }
  }

  tile->x = curx;
  tile->y = cury;
  tile->z = curz;
}

void cmd_worker()
{
  int n;
  mapcache_tile *tile;
  int z = minzoom;
  int x = grid_link->grid_limits[z].minx;
  int y = grid_link->grid_limits[z].miny;
  mapcache_context cmd_ctx = ctx;
  int nworkers = nthreads;
  if(nprocesses >= 1) nworkers = nprocesses;
  apr_pool_create(&cmd_ctx.pool,ctx.pool);
  tile = mapcache_tileset_tile_create(ctx.pool, tileset, grid_link);
  tile->dimensions = dimensions;
  if(iteration_mode == MAPCACHE_ITERATION_DEPTH_FIRST) {
    do {
      tile->x = x;
      tile->y = y;
      tile->z = z;
      cmd_recurse(&cmd_ctx,tile);
      x += tileset->metasize_x;
      if( x >= grid_link->grid_limits[z].maxx ) {
        y += tileset->metasize_y;
        if( y < grid_link->grid_limits[z].maxy) {
          x = grid_link->grid_limits[z].minx;
        }
      }
    } while (
      x < grid_link->grid_limits[z].maxx
      &&
      y < grid_link->grid_limits[z].maxy
    );
  } else {
    while(1) {
      int action;
      apr_pool_clear(cmd_ctx.pool);
      if(sig_int_received || error_detected) { //stop if we were asked to stop by hitting ctrl-c
        //remove all items from the queue
        struct seed_cmd entry;
        while (trypop_queue(&entry)!=APR_EAGAIN) {
          queuedtilestot--;
        }
        break;
      }
      tile->x = x;
      tile->y = y;
      tile->z = z;
      action = examine_tile(&cmd_ctx, tile);

      if(action == MAPCACHE_CMD_SEED || action == MAPCACHE_CMD_DELETE || action == MAPCACHE_CMD_TRANSFER) {
        //current x,y,z needs seeding, add it to the queue
        struct seed_cmd cmd;
        cmd.x = x;
        cmd.y = y;
        cmd.z = z;
        cmd.command = action;
        push_queue(cmd);
        queuedtilestot++;
        progresslog(x,y,z);
      }

      //compute next x,y,z
      x += tileset->metasize_x;
      if(x >= grid_link->grid_limits[z].maxx) {
        //x is too big, increment y
        y += tileset->metasize_y;
        if(y >= grid_link->grid_limits[z].maxy) {
          //y is too big, increment z
          z += 1;
          if(z > maxzoom) break; //we've finished seeding
          y = grid_link->grid_limits[z].miny; //set y to the smallest value for current z
        }
        x = grid_link->grid_limits[z].minx; //set x to smallest value for current z
      }
    }
  }
  //instruct rendering threads to stop working

  for(n=0; n<nworkers; n++) {
    struct seed_cmd cmd;
    cmd.command = MAPCACHE_CMD_STOP;
    push_queue(cmd);
  }

  if(error_detected && ctx.get_error_message(&ctx)) {
    printf("%s\n",ctx.get_error_message(&ctx));
  }
}


void seed_worker()
{
  mapcache_tile *tile;
  mapcache_context seed_ctx = ctx;
  apr_pool_t *tpool;
  seed_ctx.log = seed_log;
  apr_pool_create(&seed_ctx.pool,ctx.pool);
  apr_pool_create(&tpool,ctx.pool);
  tile = mapcache_tileset_tile_create(tpool, tileset, grid_link);
  tile->dimensions = dimensions;
  while(1) {
    struct seed_cmd cmd;
    apr_status_t ret;
    apr_pool_clear(seed_ctx.pool);

    ret = pop_queue(&cmd);
    if(ret != APR_SUCCESS || cmd.command == MAPCACHE_CMD_STOP) break;
    tile->x = cmd.x;
    tile->y = cmd.y;
    tile->z = cmd.z;
    if(cmd.command == MAPCACHE_CMD_SEED) {
      /* aquire a lock on the metatile ?*/
      mapcache_metatile *mt = mapcache_tileset_metatile_get(&seed_ctx, tile);
      int isLocked = mapcache_lock_or_wait_for_resource(&seed_ctx, mapcache_tileset_metatile_resource_key(&seed_ctx,mt));
      if(isLocked == MAPCACHE_TRUE) {
        /* this will query the source to create the tiles, and save them to the cache */
        mapcache_tileset_render_metatile(&seed_ctx, mt);
        mapcache_unlock_resource(&seed_ctx, mapcache_tileset_metatile_resource_key(&seed_ctx,mt));
      }
    } else if (cmd.command == MAPCACHE_CMD_TRANSFER) {
      int i;
      mapcache_metatile *mt = mapcache_tileset_metatile_get(&seed_ctx, tile);
      for (i = 0; i < mt->ntiles; i++) {
        mapcache_tile *subtile = &mt->tiles[i];
        mapcache_tileset_tile_get(&seed_ctx, subtile);
        subtile->tileset = tileset_transfer;
        tileset_transfer->cache->tile_set(&seed_ctx, subtile);
      }
    } else { //CMD_DELETE
      mapcache_tileset_tile_delete(&seed_ctx,tile,MAPCACHE_TRUE);
    }
    if(seed_ctx.get_error(&seed_ctx)) {
      error_detected++;
      ctx.log(&ctx,MAPCACHE_INFO,seed_ctx.get_error_message(&seed_ctx));
    }
  }
}

#ifdef USE_FORK
int seed_process() {
  seed_worker();
  return 0;
}
#endif
static void* APR_THREAD_FUNC seed_thread(apr_thread_t *thread, void *data) {
  seed_worker();
  return NULL;
}

void
notice(const char *fmt, ...)
{
  va_list ap;

  fprintf( stdout, "NOTICE: ");

  va_start (ap, fmt);
  vfprintf( stdout, fmt, ap);
  va_end(ap);
  fprintf( stdout, "\n" );
}

void
log_and_exit(const char *fmt, ...)
{
  va_list ap;

  fprintf( stdout, "ERROR: ");

  va_start (ap, fmt);
  vfprintf( stdout, fmt, ap);
  va_end(ap);
  fprintf( stdout, "\n" );
  exit(1);
}


int usage(const char *progname, char *msg, ...)
{
  int i=0;
  if(msg) {
    va_list args;
    va_start(args,msg);
    printf("%s\n",progname);
    vprintf(msg,args);
    printf("\noptions:\n");
    va_end(args);
  }
  else
    printf("usage: %s options\n",progname);

  while(seed_options[i].name) {
    if(seed_options[i].has_arg==TRUE) {
      printf("-%c|--%s [value]: %s\n",seed_options[i].optch,seed_options[i].name, seed_options[i].description);
    } else {
      printf("-%c|--%s: %s\n",seed_options[i].optch,seed_options[i].name, seed_options[i].description);
    }
    i++;
  }
  apr_terminate();
  return 1;
}

static int isPowerOfTwo(int x)
{
  return (x & (x - 1)) == 0;
}

int main(int argc, const char **argv)
{
  /* initialize apr_getopt_t */
  apr_getopt_t *opt;
  const char *configfile=NULL;
  apr_thread_t **threads;
  apr_threadattr_t *thread_attrs;
  const char *tileset_name=NULL;
  const char *tileset_transfer_name=NULL;
  const char *grid_name = NULL;
  int *zooms = NULL;//[2];
  mapcache_extent *extent = NULL;//[4];
  int optch;
  int rv,n;
  const char *old = NULL;
  const char *optarg;
  apr_table_t *argdimensions;
  char *dimkey=NULL, *dimvalue=NULL,*key, *last, *optargcpy=NULL;
  int keyidx;
  int *metasizes = NULL;//[2];
  int metax=-1,metay=-1;
  double *extent_array = NULL;

#ifdef USE_CLIPPERS
  const char *ogr_where = NULL;
  const char *ogr_layer = NULL;
  const char *ogr_sql = NULL;
  const char *ogr_datasource = NULL;
#endif

  apr_initialize();
  (void) signal(SIGINT,handle_sig_int);
  apr_pool_create(&ctx.pool,NULL);
  mapcache_context_init(&ctx);
  ctx.process_pool = ctx.pool;
  cfg = mapcache_configuration_create(ctx.pool);
  ctx.config = cfg;
  ctx.log= mapcache_context_seeding_log;
  apr_getopt_init(&opt, ctx.pool, argc, argv);

  seededtiles=seededtilestot=queuedtilestot=0;
  mapcache_gettimeofday(&starttime,NULL);
  lastlogtime=starttime;
  argdimensions = apr_table_make(ctx.pool,3);


  /* parse the all options based on opt_option[] */
  while ((rv = apr_getopt_long(opt, seed_options, &optch, &optarg)) == APR_SUCCESS) {
    switch (optch) {
      case 'h':
        return usage(argv[0],NULL);
        break;
      case 'f':
        force = 1;
        break;
      case 'q':
        quiet = 1;
        break;
      case 'v':
        verbose = 1;
        break;
      case 'c':
        configfile = optarg;
        break;
      case 'g':
        grid_name = optarg;
        break;
      case 't':
        tileset_name = optarg;
        break;
      case 'x':
        tileset_transfer_name = optarg;
        break;
      case 'i':
        if(!strcmp(optarg,"drill-down")) {
          iteration_mode = MAPCACHE_ITERATION_DEPTH_FIRST;
        } else if(!strcmp(optarg,"level-by-level")) {
          iteration_mode = MAPCACHE_ITERATION_LEVEL_FIRST;
        } else {
          return usage(argv[0],"invalid iteration mode, expecting \"drill-down\" or \"level-by-level\"");
        }
        break;
      case 'm':
        if(!strcmp(optarg,"delete")) {
          mode = MAPCACHE_CMD_DELETE;
        } else if(!strcmp(optarg,"transfer")) {
          mode = MAPCACHE_CMD_TRANSFER;
        } else if(strcmp(optarg,"seed")) {
          return usage(argv[0],"invalid mode, expecting \"seed\", \"delete\" or \"transfer\"");
        } else {
          mode = MAPCACHE_CMD_SEED;
        }
        break;
      case 'n':
        nthreads = (int)strtol(optarg, NULL, 10);
        if(nthreads <=0 )
          return usage(argv[0], "failed to parse nthreads, expecting positive integer");
        break;
      case 'p':
#ifdef USE_FORK
        nprocesses = (int)strtol(optarg, NULL, 10);
        if(nprocesses <=0 )
          return usage(argv[0], "failed to parse nprocesses, expecting positive integer");
        break;
#else
        return usage(argv[0], "multi process seeding not available on this platform");
#endif
      case 'e':
        if ( MAPCACHE_SUCCESS != mapcache_util_extract_double_list(&ctx, (char*)optarg, ",", &extent_array, &n) ||
             n != 4 || extent_array[0] >= extent_array[2] || extent_array[1] >= extent_array[3] ) {
          return usage(argv[0], "failed to parse extent, expecting comma separated 4 doubles");
        }
        extent = apr_palloc(ctx.pool,sizeof(mapcache_extent));
        extent->minx = extent_array[0];
        extent->miny = extent_array[1];
        extent->maxx = extent_array[2];
        extent->maxy = extent_array[3];
        break;
      case 'z':
        if ( MAPCACHE_SUCCESS != mapcache_util_extract_int_list(&ctx, (char*)optarg, ",", &zooms, &n) ||
             n != 2 || zooms[0] > zooms[1]) {
          return usage(argv[0], "failed to parse zooms, expecting comma separated 2 ints");
        } else {
          minzoom = zooms[0];
          maxzoom = zooms[1];
        }
        break;
      case 'M':
        if ( MAPCACHE_SUCCESS != mapcache_util_extract_int_list(&ctx, (char*)optarg, ",", &metasizes, &n) ||
             n != 2 || metasizes[0] <= 0 || metasizes[1] <=0) {
          return usage(argv[0], "failed to parse metasize, expecting comma separated 2 positive ints (e.g. -M 8,8");
        } else {
          metax = metasizes[0];
          metay = metasizes[1];
        }
        break;
      case 'o':
        old = optarg;
        break;
      case 'D':
        optargcpy = apr_pstrdup(ctx.pool,optarg);
        keyidx = 0;
        for (key = apr_strtok(optargcpy, "=", &last); key != NULL;
             key = apr_strtok(NULL, "=", &last)) {
          if(keyidx == 0) {
            dimkey = key;
          } else {
            dimvalue = key;
          }
          keyidx++;
        }
        if(keyidx!=2 || !dimkey || !dimvalue || !*dimkey || !*dimvalue) {
          return usage(argv[0], "failed to parse dimension, expecting DIMNAME=DIMVALUE");
        }
        apr_table_set(argdimensions,dimkey,dimvalue);
        break;
#ifdef USE_CLIPPERS
      case 'd':
        ogr_datasource = optarg;
        break;
      case 's':
        ogr_sql = optarg;
        break;
      case 'l':
        ogr_layer = optarg;
        break;
      case 'w':
        ogr_where = optarg;
        break;
#endif

    }
  }
  if (rv != APR_EOF) {
    return usage(argv[0],"bad options");
  }

  if( ! configfile ) {
    return usage(argv[0],"config not specified");
  } else {
    mapcache_configuration_parse(&ctx,configfile,cfg,0);
    if(ctx.get_error(&ctx))
      return usage(argv[0],ctx.get_error_message(&ctx));
    mapcache_configuration_post_config(&ctx,cfg);
    if(ctx.get_error(&ctx))
      return usage(argv[0],ctx.get_error_message(&ctx));
  }

#ifdef USE_CLIPPERS
  if(extent && ogr_datasource) {
    return usage(argv[0], "cannot specify both extent and ogr-datasource");
  }

  if( ogr_sql && ( ogr_where || ogr_layer )) {
    return usage(argv[0], "ogr-where or ogr_layer cannot be used in conjunction with ogr-sql");
  }

  if(ogr_datasource) {
    OGRDataSourceH hDS = NULL;
    OGRLayerH layer = NULL;
    OGRRegisterAll();

    hDS = OGROpen( ogr_datasource, FALSE, NULL );
    if( hDS == NULL ) {
      printf( "OGR Open failed\n" );
      exit( 1 );
    }

    if(ogr_sql) {
      layer = OGR_DS_ExecuteSQL( hDS, ogr_sql, NULL, NULL);
      if(!layer) {
        return usage(argv[0],"aborting");
      }
    } else {
      int nLayers = OGR_DS_GetLayerCount(hDS);
      if(nLayers>1 && !ogr_layer) {
        return usage(argv[0],"ogr datastore contains more than one layer. please specify which one to use with --ogr-layer");
      } else {
        if(ogr_layer) {
          layer = OGR_DS_GetLayerByName(hDS,ogr_layer);
        } else {
          layer = OGR_DS_GetLayer(hDS,0);
        }
        if(!layer) {
          return usage(argv[0],"aborting");
        }
        if(ogr_where) {
          if(OGRERR_NONE != OGR_L_SetAttributeFilter(layer, ogr_where)) {
            return usage(argv[0],"aborting");
          }
        }

      }
    }
    if((nClippers=OGR_L_GetFeatureCount(layer, TRUE)) == 0) {
      printf("no features in provided ogr parameters, cannot continue\n");
      apr_terminate();
      exit(0);
    }


    initGEOS(notice, log_and_exit);
    clippers = (const GEOSPreparedGeometry**)malloc(nClippers*sizeof(GEOSPreparedGeometry*));


    OGRFeatureH hFeature;
    GEOSWKTReader *geoswktreader = GEOSWKTReader_create();
    OGR_L_ResetReading(layer);
    extent = apr_palloc(ctx.pool,4*sizeof(mapcache_extent));
    int f=0;
    while( (hFeature = OGR_L_GetNextFeature(layer)) != NULL ) {
      OGRGeometryH geom = OGR_F_GetGeometryRef(hFeature);
      if(!geom ||  !OGR_G_IsValid(geom)) continue;
      char *wkt;
      OGR_G_ExportToWkt(geom,&wkt);
      GEOSGeometry *geosgeom = GEOSWKTReader_read(geoswktreader,wkt);
      free(wkt);
      clippers[f] = GEOSPrepare(geosgeom);
      //GEOSGeom_destroy(geosgeom);
      OGREnvelope ogr_extent;
      OGR_G_GetEnvelope  (geom, &ogr_extent);
      if(f == 0) {
        extent->minx = ogr_extent.MinX;
        extent->miny = ogr_extent.MinY;
        extent->maxx = ogr_extent.MaxX;
        extent->maxy = ogr_extent.MaxY;
      } else {
        extent->minx = MAPCACHE_MIN(ogr_extent.MinX, extent->minx);
        extent->miny = MAPCACHE_MIN(ogr_extent.MinY, extent->miny);
        extent->maxx = MAPCACHE_MAX(ogr_extent.MaxX, extent->maxx);
        extent->maxy = MAPCACHE_MAX(ogr_extent.MaxY, extent->maxy);
      }

      OGR_F_Destroy( hFeature );
      f++;
    }
    nClippers = f;


  }
#endif

  if( ! tileset_name ) {
    return usage(argv[0],"tileset not specified");
  } else {
    tileset = mapcache_configuration_get_tileset(cfg,tileset_name);
    if(!tileset) {
      return usage(argv[0], "tileset not found in configuration");
    }
    if(tileset->read_only) {
      printf("tileset %s is read-only, switching it to read-write for seeding\n",tileset_name);
      tileset->read_only = 0;
    }
    if( ! grid_name ) {
      grid_link = APR_ARRAY_IDX(tileset->grid_links,0,mapcache_grid_link*);
    } else {
      int i;
      for(i=0; i<tileset->grid_links->nelts; i++) {
        mapcache_grid_link *sgrid = APR_ARRAY_IDX(tileset->grid_links,i,mapcache_grid_link*);
        if(!strcmp(sgrid->grid->name,grid_name)) {
          grid_link = sgrid;
          break;
        }
      }
      if(!grid_link) {
        return usage(argv[0],"grid not configured for tileset");
      }
    }
    if(iteration_mode == MAPCACHE_ITERATION_UNSET) {
      if(!strcmp(grid_link->grid->name,"g") || !strcmp(grid_link->grid->name,"WGS84")
              || !strcmp(grid_link->grid->name,"GoogleMapsCompatible")) {
        iteration_mode = MAPCACHE_ITERATION_DEPTH_FIRST;
      } else {
        iteration_mode = MAPCACHE_ITERATION_LEVEL_FIRST;
      }
    }
    if(minzoom == -1 && maxzoom == -1) {
      minzoom = grid_link->minz;
      maxzoom = grid_link->maxz - 1;
    }
    if(minzoom<grid_link->minz) minzoom = grid_link->minz;
    if(maxzoom>= grid_link->maxz) maxzoom = grid_link->maxz - 1;
    if(grid_link->outofzoom_strategy != MAPCACHE_OUTOFZOOM_NOTCONFIGURED && maxzoom > grid_link->max_cached_zoom) {
      maxzoom = grid_link->max_cached_zoom;
    }

    /* adjust metasize */
    if(metax>0) {
      tileset->metasize_x = metax;
      tileset->metasize_y = metay;
    }

    /* ensure our metasize is a power of 2 in drill down mode */
    if(iteration_mode == MAPCACHE_ITERATION_DEPTH_FIRST) {
      if(!isPowerOfTwo(tileset->metasize_x) || !isPowerOfTwo(tileset->metasize_y)) {
        return usage(argv[0],"metatile size is not set to a power of two and iteration mode set to \"drill-down\", rerun with e.g -M 8,8, or force iteration mode to \"level-by-level\"");
      }
    }


  }

  if (mode == MAPCACHE_CMD_TRANSFER) {
    if (!tileset_transfer_name)
      return usage(argv[0],"tileset where tiles should be transferred to not specified");

    tileset_transfer = mapcache_configuration_get_tileset(cfg,tileset_transfer_name);
    if(!tileset_transfer)
      return usage(argv[0], "tileset where tiles should be transferred to not found in configuration");
  }

  if(old) {
    if(strcasecmp(old,"now")) {
      struct tm oldtime;
      char *ret;
      memset(&oldtime,0,sizeof(oldtime));
      ret = strptime(old,"%Y/%m/%d %H:%M",&oldtime);
      if(!ret || *ret) {
        return usage(argv[0],"failed to parse time");
      }
      if(APR_SUCCESS != apr_time_ansi_put(&age_limit,mktime(&oldtime))) {
        return usage(argv[0],"failed to convert time");
      }
    } else {
      age_limit = apr_time_now();
    }
  }

  if(extent) {
    // update the grid limits
    mapcache_grid_compute_limits(grid_link->grid,extent,grid_link->grid_limits,0);
  }

  /* adjust our grid limits so they align on the metatile limits
   * we need to do this because the seeder does not check for individual tiles, it
   * goes from one metatile to the next*/
  for(n=0; n<grid_link->grid->nlevels; n++) {
    if(tileset->metasize_x > 1) {
      grid_link->grid_limits[n].minx = (grid_link->grid_limits[n].minx/tileset->metasize_x)*tileset->metasize_x;
      grid_link->grid_limits[n].maxx = (grid_link->grid_limits[n].maxx/tileset->metasize_x+1)*tileset->metasize_x;
      if( grid_link->grid_limits[n].maxx > grid_link->grid->levels[n]->maxx)
        grid_link->grid_limits[n].maxx = grid_link->grid->levels[n]->maxx;
    }
    if(tileset->metasize_y > 1) {
      grid_link->grid_limits[n].miny = (grid_link->grid_limits[n].miny/tileset->metasize_y)*tileset->metasize_y;
      grid_link->grid_limits[n].maxy = (grid_link->grid_limits[n].maxy/tileset->metasize_y+1)*tileset->metasize_y;
      if( grid_link->grid_limits[n].maxy > grid_link->grid->levels[n]->maxy)
        grid_link->grid_limits[n].maxy = grid_link->grid->levels[n]->maxy;
    }
  }

  /* validate the supplied dimensions */
  if (!apr_is_empty_array(tileset->dimensions) || tileset->timedimension) {
    int i;
    const char *value;
    dimensions = apr_table_make(ctx.pool,3);
    if (!apr_is_empty_array(tileset->dimensions)) {
      for(i=0; i<tileset->dimensions->nelts; i++) {
        mapcache_dimension *dimension = APR_ARRAY_IDX(tileset->dimensions,i,mapcache_dimension*);
        if((value = (char*)apr_table_get(argdimensions,dimension->name)) != NULL) {
          char *tmpval = apr_pstrdup(ctx.pool,value);
          int ok = dimension->validate(&ctx,dimension,&tmpval);
          if(GC_HAS_ERROR(&ctx) || ok != MAPCACHE_SUCCESS ) {
            return usage(argv[0],"failed to validate dimension");
            return 1;
          } else {
            /* validate may have changed the dimension value, so set that value into the dimensions table */
            apr_table_setn(dimensions,dimension->name,tmpval);
          }
        } else {
          /* a dimension was not specified on the command line, add the default value */
          apr_table_setn(dimensions, dimension->name, dimension->default_value);
        }
      }
    }
    if(tileset->timedimension) {
      if((value = (char*)apr_table_get(argdimensions,tileset->timedimension->key)) != NULL) {
        apr_array_header_t *timedim_selected = mapcache_timedimension_get_entries_for_value(&ctx,tileset->timedimension, tileset, grid_link->grid, extent, value);
        if(GC_HAS_ERROR(&ctx) || !timedim_selected) {
          return usage(argv[0],"failed to validate time dimension");
        }
        if(timedim_selected->nelts == 0) {
          return usage(argv[0],"Time dimension %s=%s returns no configured entry",tileset->timedimension->key,
                  value);
        }
        if(timedim_selected->nelts > 1) {
          return usage(argv[0],"Time dimension %s=%s returns more than 1 configured entries",tileset->timedimension->key,
                  value);
        }
        apr_table_set(dimensions,tileset->timedimension->key,APR_ARRAY_IDX(timedim_selected,0,char*));
      } else {
        return usage(argv[0],"tileset references a TIME dimension, but none supplied on commandline. (hint: -D %s=<timestamp>",tileset->timedimension->key);
        
      }
    }
  }

  if(nthreads == 0 && nprocesses == 0) {
    nthreads = 1;
  }
  if(nthreads >= 1 && nprocesses >= 1) {
    return usage(argv[0],"cannot set both nthreads and nprocesses");
  }
  if(nprocesses > 1) {
#ifdef USE_FORK
    key_t key;
    int i;
    pid_t *pids = malloc(nprocesses*sizeof(pid_t));
    struct msqid_ds queue_ds;
    ctx.threadlock = NULL;
    key = ftok(argv[0], 'B');
    if ((msqid = msgget(key, 0644 | IPC_CREAT|S_IRUSR|S_IWUSR)) == -1) {
      return usage(argv[0],"failed to create sysv ipc message queue");
    }
    if (-1 == msgctl(msqid, IPC_STAT, &queue_ds)) {
      return usage(argv[0], "\nFailure in msgctl() stat");
    }
    queue_ds.msg_qbytes = nprocesses*sizeof(struct seed_cmd);
    if(-1 == msgctl(msqid, IPC_SET, &queue_ds)) {
      switch(errno) {
        case EACCES:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: EACCESS (should not happen here)");
        case EFAULT:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: EFAULT queue not accessible");
        case EIDRM:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: EIDRM message queue removed");
        case EINVAL:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: EINVAL invalid value for msg_qbytes");
        case EPERM:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: EPERM permission denied on msg_qbytes");
        default:
          return usage(argv[0], "\nFailure in msgctl() set qbytes: unknown");
      }
    }

    for(i=0; i<nprocesses; i++) {
      int pid = fork();
      if(pid==0) {
        seed_process();
        exit(0);
      } else {
        pids[i] = pid;
      }
    }
    cmd_worker();
    for(i=0; i<nprocesses; i++) {
      int stat_loc;
      waitpid(pids[i],&stat_loc,0);
    }
    msgctl(msqid,IPC_RMID,NULL);
#else
    return usage(argv[0],"bug: multi process support not available");
#endif
  } else {
    //start the thread that will populate the queue.
    apr_thread_mutex_create((apr_thread_mutex_t**)&ctx.threadlock,APR_THREAD_MUTEX_DEFAULT,ctx.pool);
    //create the queue where tile requests will be put
    apr_queue_create(&work_queue,nthreads,ctx.pool);

    //start the rendering threads.
    apr_threadattr_create(&thread_attrs, ctx.pool);
    threads = (apr_thread_t**)apr_pcalloc(ctx.pool, nthreads*sizeof(apr_thread_t*));
    for(n=0; n<nthreads; n++) {
      apr_thread_create(&threads[n], thread_attrs, seed_thread, NULL, ctx.pool);
    }
    cmd_worker();
    for(n=0; n<nthreads; n++) {
      apr_thread_join(&rv, threads[n]);
    }
  }
  if(ctx.get_error(&ctx)) {
    printf("%s",ctx.get_error_message(&ctx));
  }

  if(seededtilestot>0) {
    struct mctimeval now_t;
    float duration;
    mapcache_gettimeofday(&now_t,NULL);
    duration = ((now_t.tv_sec-starttime.tv_sec)*1000000+(now_t.tv_usec-starttime.tv_usec))/1000000.0;
    printf("\nseeded %d metatiles at %g tiles/sec\n",seededtilestot, seededtilestot/duration);
  }
  apr_terminate();

  if (error_detected > 0) {
    exit(1);  
  }
  
  return 0;
}
/* vim: ts=2 sts=2 et sw=2
*/
