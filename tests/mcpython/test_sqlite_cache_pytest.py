# Project:  MapCache
# Purpose:  Test MapCache SQLite based storage backend
# Author:   Maris Nartiss
#
# *****************************************************************************
# Copyright (c) 2025 Regents of the University of Minnesota.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies of this Software or works derived from this Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
# ****************************************************************************/

import os
import pytest
import numpy as np
import sqlite3
import logging

from osgeo import gdal

# Import the GeoTIFF generation function
from generate_synthetic_geotiff import generate_synthetic_geotiff

# Import generic verification functions and constants
from verification_core import (
    TILE_SIZE,
    TILE_CACHE_BASE_DIR,
    TEMP_MAPCACHE_CONFIG_DIR,
    calculate_expected_tile_data,
    compare_tile_arrays,
    cleanup,
    run_seeder,
    create_temp_mapcache_config,
)

# --- Configuration --- #
SYNTHETIC_GEOTIFF_FILENAME = os.path.join(
    TEMP_MAPCACHE_CONFIG_DIR, "synthetic_test_data.tif"
)
GEOTIFF_WIDTH = 512
GEOTIFF_HEIGHT = 512
MAPCACHE_TEMPLATE_CONFIG = os.path.join(
    os.path.dirname(__file__), "..", "data", "mapcache_backend_template.xml"
)

# --- Grid Parameters --- #
INITIAL_RESOLUTION = 1000
ORIGIN_X = -500000
ORIGIN_Y = 500000


def read_tile(zoom, x, y, tile_size=TILE_SIZE):
    db_file = os.path.join(TILE_CACHE_BASE_DIR, "cache.sqlite")
    tmp_tile = os.path.join(TILE_CACHE_BASE_DIR, "temp_tile_sqlite.png")
    if not os.path.exists(db_file):
        logging.error(f"Error: Database file not found at {db_file}")
        return None

    try:
        con = sqlite3.connect(db_file)
        cur = con.cursor()
        cur.execute(
            "SELECT data FROM tiles WHERE tileset='sqlite-tileset' AND "
            "grid='synthetic_grid' AND x=? AND y=? AND z=?",
            (x, y, zoom),
        )
        row = cur.fetchone()
        if row is None:
            logging.error(f"Error: Tile not found in database for Z{zoom}-X{x}-Y{y}")
            return None

        # Assume SQLite returned raw PNG data
        with open(tmp_tile, "wb") as f:
            f.write(row[0])

        actual_ds = gdal.Open(tmp_tile, gdal.GA_ReadOnly)
        if actual_ds is None:
            logging.error(
                f"Error: Could not open tile from database data. Tile: {tmp_tile}"
            )
            return None

        # Read all bands from the actual tile
        actual_tile_data = np.zeros(
            (tile_size, tile_size, actual_ds.RasterCount), dtype=np.uint8
        )
        for i in range(actual_ds.RasterCount):
            actual_tile_data[:, :, i] = actual_ds.GetRasterBand(i + 1).ReadAsArray()

        actual_ds = None  # Close the dataset

        # Mapcache might output 4 bands (RGBA) even if source is 3 bands. Handle this.
        if actual_tile_data.shape[2] == 4:
            actual_tile_data_rgb = actual_tile_data[:, :, :3]  # Take only RGB bands
        elif actual_tile_data.shape[2] == 3:
            actual_tile_data_rgb = actual_tile_data
        else:
            logging.error(
                f"Error: Unexpected number of bands in actual tile: {actual_tile_data.shape[2]}"
            )
            return None

        return actual_tile_data_rgb

    except sqlite3.Error as e:
        logging.error(f"Database error: {e}")
        return None
    finally:
        if con:
            con.close()


def run_mapcache_test(zoom, x, y, geotiff_path, initial_resolution, origin_x, origin_y):
    logging.info(f"Running MapCache test for tile Z{zoom}-X{x}-Y{y}...")

    # Calculate expected tile data using generic function
    expected_tile_data = calculate_expected_tile_data(
        zoom,
        x,
        y,
        geotiff_path,
        initial_resolution,
        origin_x,
        origin_y,
    )
    if expected_tile_data is None:
        return False

    # --- Read Actual Tile Data ---
    actual_tile_data_rgb = read_tile(zoom, x, y, TILE_SIZE)
    if actual_tile_data_rgb is None:
        return False

    # --- Compare ---
    return compare_tile_arrays(expected_tile_data, actual_tile_data_rgb, zoom, x, y)


@pytest.fixture(scope="module")
def setup_test_environment(request):
    cleanup()
    logging.info("Testing sqlite storage backend...")
    os.makedirs(TEMP_MAPCACHE_CONFIG_DIR, exist_ok=True)
    generate_synthetic_geotiff(
        output_filename=SYNTHETIC_GEOTIFF_FILENAME,
        width=GEOTIFF_WIDTH,
        height=GEOTIFF_HEIGHT,
    )
    create_temp_mapcache_config(
        SYNTHETIC_GEOTIFF_FILENAME,
        MAPCACHE_TEMPLATE_CONFIG,
    )
    run_seeder("sqlite-tileset", "0,1")

    def teardown():
        cleanup()
        logging.info("Cleanup complete.")

    request.addfinalizer(teardown)


def test_sqlite_tiles(setup_test_environment):
    ok0 = run_mapcache_test(
        0,
        0,
        0,
        SYNTHETIC_GEOTIFF_FILENAME,
        INITIAL_RESOLUTION,
        ORIGIN_X,
        ORIGIN_Y,
    )
    ok1 = run_mapcache_test(
        1,
        1,
        2,
        SYNTHETIC_GEOTIFF_FILENAME,
        INITIAL_RESOLUTION,
        ORIGIN_X,
        ORIGIN_Y,
    )
    assert ok0
    assert ok1
