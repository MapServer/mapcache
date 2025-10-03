# Project:  MapCache
# Purpose:  Generates a GeoTIFF with a predictable content to serve as a reference
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

import numpy as np
import logging

from osgeo import gdal, osr


def generate_synthetic_geotiff(
    output_filename="synthetic_test_data.tif", width=256, height=256
):
    """
    Generates a synthetic GeoTIFF with unique pixel values based on their coordinates.
    Each pixel value encodes its row and column index, allowing for detection of
    shift and rotation errors.
    """
    osr.DontUseExceptions()
    # Define image properties
    wm_min_x = -500000
    wm_max_x = 500000
    wm_min_y = -500000
    wm_max_y = 500000

    pixel_width = (wm_max_x - wm_min_x) / width
    pixel_height = (wm_min_y - wm_max_y) / height  # Negative for north-up image

    # GeoTransform: [top-left x, pixel width, 0, top-left y, 0, pixel height]
    # Top-left corner is (wm_min_x, wm_max_y)
    geotransform = [wm_min_x, pixel_width, 0, wm_max_y, 0, pixel_height]

    # Spatial Reference System (Web Mercator)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(3857)

    # Determine appropriate data type based on image dimensions
    # For 3-band output, we'll use uint8 for each band.
    gdal_datatype = gdal.GDT_Byte
    numpy_datatype = np.uint8
    num_bands = 3

    # Create the GeoTIFF file
    driver = gdal.GetDriverByName("GTiff")
    dataset = driver.Create(output_filename, width, height, num_bands, gdal_datatype)

    if dataset is None:
        logging.error(f"Error: Could not create {output_filename}")
        return

    dataset.SetGeoTransform(geotransform)
    dataset.SetSpatialRef(srs)

    # Create NumPy arrays to hold the pixel data for each band
    data_band1 = np.zeros((height, width), dtype=numpy_datatype)
    data_band2 = np.zeros((height, width), dtype=numpy_datatype)
    data_band3 = np.zeros((height, width), dtype=numpy_datatype)

    # Generate unique pixel values based on row and column index for each band
    # Band 1 (Red): row % 256
    # Band 2 (Green): col % 256
    # Band 3 (Blue): (row + col) % 256
    for row in range(height):
        for col in range(width):
            data_band1[row, col] = row % 256
            data_band2[row, col] = col % 256
            data_band3[row, col] = (row + col) % 256

    # Write the data to each band
    dataset.GetRasterBand(1).WriteArray(data_band1)
    dataset.GetRasterBand(2).WriteArray(data_band2)
    dataset.GetRasterBand(3).WriteArray(data_band3)

    # Close the dataset
    dataset = None
    logging.info(f"Successfully created synthetic GeoTIFF: {output_filename}")
