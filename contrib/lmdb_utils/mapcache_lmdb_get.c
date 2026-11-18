/******************************************************************************
 * 
 * Project:  MapCache
 * Purpose:  Extract raw value of a single key from a LMDB database
 * Author:   Maris Nartiss
 * 
 ******************************************************************************
 * Copyright (c) 2025 Regents of the University of Minnesota.
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
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <apr_time.h>
#include <apr_getopt.h>
#include <apr_pools.h>
#include <apr_date.h>
#include "lmdb.h"

void fail(const char *msg, int rc) {
    fprintf(stderr, "%s: %s\n", msg, mdb_strerror(rc));
    exit(EXIT_FAILURE);
}

static const apr_getopt_option_t options[] = {
    { "dbpath", 'd', TRUE, "Path to the LMDB database directory" },
    { "key", 'k', TRUE, "Key to retrieve" },
    { "output", 'o', TRUE, "Output file (default: output.bin)" },
    { "help", 'h', FALSE, "Show help" },
    { NULL, 0, 0, NULL }
};

void usage(const char *progname) {
    int i = 0;
    printf("usage: %s options\n", progname);
    while (options[i].name) {
        if (options[i].optch < 256) {
            if (options[i].has_arg == TRUE) {
                printf("-%c|--%s [value]: %s\n", options[i].optch, options[i].name, options[i].description);
            } else {
                printf("-%c|--%s: %s\n", options[i].optch, options[i].name, options[i].description);
            }
        } else {
            if (options[i].has_arg == TRUE) {
                printf("   --%s [value]: %s\n", options[i].name, options[i].description);
            } else {
                printf("   --%s: %s\n", options[i].name, options[i].description);
            }
        }
        i++;
    }
    exit(EXIT_FAILURE);
}

int main(int argc, const char * const *argv) {
    const char *db_path = NULL;
    const char *key_str = NULL;
    const char *out_filename = "output.bin";
    int rc;
    MDB_env *env;
    MDB_txn *txn;
    MDB_dbi dbi;
    MDB_val key, data;
    apr_pool_t *pool = NULL;
    apr_getopt_t *opt;
    int optch;
    const char *optarg;

    apr_initialize();
    apr_pool_create(&pool, NULL);
    apr_getopt_init(&opt, pool, argc, argv);

    while ((rc = apr_getopt_long(opt, options, &optch, &optarg)) == APR_SUCCESS) {
        switch (optch) {
            case 'h':
                usage(argv[0]);
                break;
            case 'd':
                db_path = optarg;
                break;
            case 'k':
                key_str = optarg;
                break;
            case 'o':
                out_filename = optarg;
                break;
        }
    }

    if (rc != APR_EOF) {
        fprintf(stderr, "Error: Invalid option.\n");
        usage(argv[0]);
    }

    if (!db_path || !key_str) {
        fprintf(stderr, "Error: --dbpath and --key are required.\n");
        usage(argv[0]);
    }

    // Create and open the environment
    rc = mdb_env_create(&env);
    if (rc) fail("mdb_env_create", rc);

    rc = mdb_env_open(env, db_path, MDB_RDONLY, 0664);
    if (rc) {
        mdb_env_close(env);
        fail("mdb_env_open", rc);
    }

    // Begin a read-only transaction
    rc = mdb_txn_begin(env, NULL, MDB_RDONLY, &txn);
    if (rc) {
        mdb_env_close(env);
        fail("mdb_txn_begin", rc);
    }

    // Open the database
    rc = mdb_dbi_open(txn, NULL, 0, &dbi);
    if (rc) {
        mdb_txn_abort(txn);
        mdb_env_close(env);
        fail("mdb_dbi_open", rc);
    }

    // Prepare the key, which is a null-terminated string in the DB
    key.mv_size = strlen(key_str) + 1;
    key.mv_data = (void *)key_str;

    // Retrieve the data
    rc = mdb_get(txn, dbi, &key, &data);

    if (rc == MDB_SUCCESS) {
        apr_time_t timestamp;
        char time_str[APR_RFC822_DATE_LEN];
        FILE *fp;
        fp = fopen(out_filename, "wb");
        if (!fp) {
            mdb_txn_abort(txn);
            mdb_env_close(env);
            perror("Unable to open output file");
            return EXIT_FAILURE;
        } else {
            // Check for special blank tile encoding ('#' marker)
            if (data.mv_size > 1 && ((char *)data.mv_data)[0] == '#') {
                if (data.mv_size == 5 + sizeof(apr_time_t)) {
                    const unsigned char *color = (unsigned char *)data.mv_data + 1;
                    printf("Key found. Blank tile, color #%02x%02x%02x%02x. "
                           "Writing description to \"%s\"\n",
                           color[0], color[1], color[2], color[3], out_filename);
                    fprintf(fp, "Blank tile, RGBA: #%02x%02x%02x%02x\n",
                            color[0], color[1], color[2], color[3]);
                } else {
                    printf("Key found. Blank tile marker found, but data "
                           "size is unexpected (%zu bytes). Writing as is.\n",
                           data.mv_size);
                    fwrite(data.mv_data, 1, data.mv_size, fp);
                }
                
                // Extract timestamp from the end of the data
                memcpy(&timestamp, (char *)data.mv_data + 5, sizeof(apr_time_t));

                // Convert to human-readable format
                apr_rfc822_date(time_str, timestamp);
                
                printf("Timestamp: %s\n", time_str);
            } else if (data.mv_size >= sizeof(apr_time_t)) {
                // Regular tile data, strip the trailing timestamp
                size_t image_size = data.mv_size - sizeof(apr_time_t);

                // Extract timestamp from the end of the data
                memcpy(&timestamp, (char *)data.mv_data + image_size, sizeof(apr_time_t));

                // Convert to human-readable format
                apr_rfc822_date(time_str, timestamp);

                printf("Key found. Writing %zu image bytes to \"%s\"\n",
                       image_size, out_filename);
                printf("Timestamp: %s\n", time_str);
                fwrite(data.mv_data, 1, image_size, fp);
            } else {
                // Data is smaller than a timestamp, write as is
                printf("Key found. Data size (%zu) is smaller than a "
                       "timestamp, writing as is to \"%s\"\n",
                       data.mv_size, out_filename);
                fwrite(data.mv_data, 1, data.mv_size, fp);
            }
            fclose(fp);
        }
    } else if (rc == MDB_NOTFOUND) {
        fprintf(stderr, "Key '%s' not found.\n", key_str);
    } else {
        fail("mdb_get", rc);
    }

    // Clean up
    mdb_txn_abort(txn);
    mdb_env_close(env);
    
    apr_pool_destroy(pool);
    apr_terminate();

    return (rc == MDB_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
