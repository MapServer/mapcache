# Project:  MapCache
# Purpose:  Common code for various MapCache storage backend tests
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
import shutil
import math
import subprocess
import numpy as np
import logging

from osgeo import gdal

TILE_SIZE = 256
TEMP_MAPCACHE_CONFIG_DIR = "/tmp/mc_test"
TEMP_MAPCACHE_CONFIG_FILE = os.path.join(TEMP_MAPCACHE_CONFIG_DIR, "mapcache.xml")
TILE_CACHE_BASE_DIR = os.path.join(TEMP_MAPCACHE_CONFIG_DIR, "cache_data")


def calculate_expected_tile_data(
    zoom, x, y, geotiff_path, initial_resolution, origin_x, origin_y
):
    """
    Calculates the expected pixel data for a given tile (zoom, x, y)
    by reading directly from the source GeoTIFF using GDAL,
    thus matching any resampling done by MapCache.
    Returns a 3-band NumPy array (uint8) or None on error.
    """
    # Calculate resolution for the current zoom level
    resolution = initial_resolution / (2**zoom)

    # Calculate geographic bounds of the tile
    min_x_tile = origin_x + x * TILE_SIZE * resolution
    max_y_tile = origin_y - y * TILE_SIZE * resolution
    max_x_tile = min_x_tile + TILE_SIZE * resolution
    min_y_tile = max_y_tile - TILE_SIZE * resolution

    logging.info(
        "Tile geographic bounds (Web Mercator):\n  "
        f"MinX: {min_x_tile}, MinY: {min_y_tile}\n  "
        f"MaxX: {max_x_tile}, MaxY: {max_y_tile}"
    )

    expected_tile_data = np.zeros((TILE_SIZE, TILE_SIZE, 3), dtype=np.uint8)

    # Open the source GeoTIFF
    src_ds = gdal.Open(geotiff_path, gdal.GA_ReadOnly)
    if src_ds is None:
        logging.error(f"Error: Could not open source GeoTIFF {geotiff_path}")
        return None

    geotiff_width = src_ds.RasterXSize
    geotiff_height = src_ds.RasterYSize

    src_gt = src_ds.GetGeoTransform()

    # Get the source bands
    src_bands = [src_ds.GetRasterBand(i + 1) for i in range(src_ds.RasterCount)]

    # Iterate over each pixel in the output tile
    for py in range(TILE_SIZE):
        for px in range(TILE_SIZE):
            # Calculate geographic coordinates (Web Mercator) of the center of the pixel in the tile
            map_x = min_x_tile + (px + 0.5) * resolution
            map_y = max_y_tile - (py + 0.5) * resolution

            # Map Web Mercator (x, y) to pixel (col, row) in the source GeoTIFF
            src_col = math.floor((map_x - src_gt[0]) / src_gt[1])
            src_row = math.floor((map_y - src_gt[3]) / src_gt[5])

            # Read pixel value directly from the source GeoTIFF
            if 0 <= src_row < geotiff_height and 0 <= src_col < geotiff_width:
                for band_idx in range(3):  # Assuming 3 bands (RGB)
                    # ReadRaster(xoff, yoff, xsize, ysize, buf_xsize, buf_ysize, buf_type)
                    # Read a single pixel
                    val = src_bands[band_idx].ReadRaster(
                        src_col, src_row, 1, 1, 1, 1, gdal.GDT_Byte
                    )
                    expected_tile_data[py, px, band_idx] = np.frombuffer(
                        val, dtype=np.uint8
                    )[0]
            else:
                # If the coordinate falls outside the source GeoTIFF, set to 0 (black)
                expected_tile_data[py, px, :] = 0

    src_ds = None  # Close the source dataset
    return expected_tile_data


def compare_tile_arrays(expected_data, actual_data, zoom, x, y):
    """
    Compares two NumPy arrays representing tile data and reports discrepancies.
    Returns True if arrays are equal, False otherwise.
    """
    if actual_data is None:
        return False

    if np.array_equal(expected_data, actual_data):
        logging.info(f"SUCCESS: Tile Z{zoom}-X{x}-Y{y} matches expected data.")
        return True
    else:
        logging.error(f"FAILURE: Tile Z{zoom}-X{x}-Y{y} does NOT match expected data.")
        diff = expected_data.astype(np.int16) - actual_data.astype(np.int16)
        logging.error("Differences (Expected - Actual):\n%s", diff)
        diff_coords = np.argwhere(diff != 0)
        if len(diff_coords) > 0:
            logging.error("First 10 differing pixel coordinates and values:")
            for coord in diff_coords[:10]:
                py, px, band_idx = coord
                logging.error(
                    f"  Pixel ({px}, {py}), Band {band_idx}: "
                    f"Expected={expected_data[py, px, band_idx]}, "
                    f"Actual={actual_data[py, px, band_idx]}"
                )
        return False


def cleanup():
    if os.path.exists(TEMP_MAPCACHE_CONFIG_DIR):
        shutil.rmtree(TEMP_MAPCACHE_CONFIG_DIR)


def create_temp_mapcache_config(geotiff_path, mapcache_template_config):
    """
    Replace dynamic parts of config file with actual values
    """

    os.makedirs(TEMP_MAPCACHE_CONFIG_DIR, exist_ok=True)

    with open(mapcache_template_config, "r") as f:
        template_content = f.read()

    content = template_content.replace(
        "SYNTHETIC_GEOTIFF_PATH_PLACEHOLDER", geotiff_path
    )
    content = content.replace("TILE_CACHE_BASE_DIR", TILE_CACHE_BASE_DIR)

    with open(TEMP_MAPCACHE_CONFIG_FILE, "w") as f:
        f.write(content)

    logging.info(f"Created temporary mapcache config: {TEMP_MAPCACHE_CONFIG_FILE}")


def run_seeder(tileset, zoomlevels):
    """
    Prepopulate storage backend with tiles
    Tileset is a tileset name
    Zoomlevels – a string with zoomlevels to seed e.g. "0,2"
    """

    logging.info("Running mapcache seeder...")
    seeder_command = [
        "mapcache_seed",
        "-c",
        TEMP_MAPCACHE_CONFIG_FILE,
        "-t",
        tileset,
        "--force",
        "-z",
        zoomlevels,
    ]
    try:
        result = subprocess.run(
            seeder_command, check=True, capture_output=True, text=True
        )
        logging.info("Seeder stdout: %s", result.stdout)
        if result.stderr:
            logging.error("Seeder stderr: %s", result.stderr)
        logging.info("Mapcache seeder finished.")
    except subprocess.CalledProcessError as e:
        logging.error(
            f"Error running mapcache_seed: {e} Temporary files in: {TEMP_MAPCACHE_CONFIG_DIR}"
        )
        logging.error("Stdout: %s", e.stdout)
        logging.error("Stderr: %s", e.stderr)
        exit(1)
