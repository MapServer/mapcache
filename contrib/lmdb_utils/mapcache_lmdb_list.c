/******************************************************************************
 * 
 * Project:  MapCache
 * Purpose:  List all keys in a LMDB database
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
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <apr_getopt.h>
#include <apr_pools.h>
#include <apr_time.h>
#include <apr_date.h>
#include <apr_strings.h>
#include "lmdb.h"

void fail(const char *msg, int rc) {
    fprintf(stderr, "%s: %s\n", msg, mdb_strerror(rc));
    exit(EXIT_FAILURE);
}

static const apr_getopt_option_t options[] = {
    { "dbpath", 'd', TRUE, "Path to the LMDB database directory" },
    { "summary", 's', FALSE, "Print only the total number of keys" },
    { "extended", 'e', FALSE, "Show extended info (timestamp, size)" },
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
    int rc;
    MDB_env *env;
    MDB_txn *txn;
    MDB_cursor *cursor = NULL;
    MDB_val key;
    MDB_dbi dbi;
    MDB_val value;
    int summary_flag = 0;
    int extended_flag = 0;
    MDB_stat stat;
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
            case 's':
                summary_flag = 1;
                break;
            case 'e':
                extended_flag = 1;
                break;
        }
    }

    if (rc != APR_EOF) {
        fprintf(stderr, "Error: Invalid option.\n");
        usage(argv[0]);
    }

    if (!db_path) {
        fprintf(stderr, "Error: --dbpath is required.\n");
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

    if (summary_flag) {
        rc = mdb_stat(txn, dbi, &stat);
        if (rc) {
          mdb_txn_abort(txn);
          mdb_env_close(env);
          fail("mdb_stat", rc);
        }
        printf("Total keys found: %zu\n", stat.ms_entries);
    } else {
        rc = mdb_cursor_open(txn, dbi, &cursor);
        if (rc) {
            mdb_txn_abort(txn);
            mdb_env_close(env);
            fail("mdb_cursor_open", rc);
        }

        // Iterate over keys
        while ((rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT)) == 0) {
            // Print the key itself, without the null terminator
            fwrite(key.mv_data, 1, key.mv_size - 1, stdout);

            if (extended_flag) {
                apr_time_t timestamp;
                size_t data_size;

                // Check for special blank tile encoding ('#' marker)
                if (value.mv_size > 1 && ((char *)value.mv_data)[0] == '#') {
                    if (value.mv_size == 5 + sizeof(apr_time_t)) {
                        memcpy(&timestamp, (char *)value.mv_data + 5, sizeof(apr_time_t));
                        data_size = 4; // RGBA color
                    } else {
                        // Unexpected size, cannot parse
                        timestamp = 0;
                        data_size = 0;
                    }
                } else if (value.mv_size >= sizeof(apr_time_t)) {
                    // Regular tile data
                    data_size = value.mv_size - sizeof(apr_time_t);
                    memcpy(&timestamp, (char *)value.mv_data + data_size, sizeof(apr_time_t));
                } else {
                    // Data is too small, cannot parse
                    timestamp = 0;
                    data_size = value.mv_size;
                }

                if (timestamp > 0) {
                    apr_time_exp_t exploded_time;
                    char iso_time_str[30];
                    apr_time_exp_gmt(&exploded_time, timestamp);
                    apr_snprintf(iso_time_str, sizeof(iso_time_str),
                                 "%d-%02d-%02dT%02d:%02d:%02dZ",
                                 exploded_time.tm_year + 1900,
                                 exploded_time.tm_mon + 1,
                                 exploded_time.tm_mday,
                                 exploded_time.tm_hour,
                                 exploded_time.tm_min,
                                 exploded_time.tm_sec);
                    printf(",%s,%zu", iso_time_str, data_size);
                } else {
                    printf(",,%zu", data_size); // Print size even if timestamp is unknown
                }
            }
            printf("\n");
        }

        if (rc != MDB_NOTFOUND) {
            mdb_txn_abort(txn);
            mdb_env_close(env);
            fail("mdb_cursor_get", rc);
        }
        mdb_cursor_close(cursor);
    }

    // Clean up
    mdb_txn_abort(txn);
    mdb_env_close(env);

    apr_pool_destroy(pool);
    apr_terminate();

    return EXIT_SUCCESS;
}
