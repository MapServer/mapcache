#!/bin/sh

# Project:  MapCache
# Purpose:  MapCache tests
# Author:   Even Rouault, Maris Nartiss
#
#*****************************************************************************
# Copyright (c) 2017, 2025 Regents of the University of Minnesota.
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
#****************************************************************************/

set -e

MAPCACHE_CONF=/tmp/mc/mapcache.xml

rm -rf /tmp/mc/global
mapcache_seed -c /tmp/mc/mapcache.xml -t global --force -z 0,1
gdalinfo /tmp/mc/global/GoogleMapsCompatible/00/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size"; gdalinfo /tmp/mc/global/GoogleMapsCompatible/00/000/000/000/000/000/000.jpg; /bin/false)
rm -rf /tmp/mc/global

curl -s "http://localhost/mapcache/?SERVICE=WMS&REQUEST=GetCapabilities" | xmllint --format - > /tmp/wms_capabilities.xml
diff -u /tmp/wms_capabilities.xml expected

curl -s "http://localhost/mapcache/wmts?SERVICE=WMTS&REQUEST=GetCapabilities" | xmllint --format - > /tmp/wmts_capabilities.xml
diff -u /tmp/wmts_capabilities.xml expected

curl -s "http://localhost/mapcache/wmts/1.0.0/global/default/GoogleMapsCompatible/0/0/0.jpg" > /tmp/0.jpg
gdalinfo /tmp/0.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size"; gdalinfo /tmp/0.jpg; /bin/false)

curl -s "http://localhost/mapcache/wmts/1.0.0/global/default/GoogleMapsCompatible/0/0/0.jpg" > /tmp/0_bis.jpg
diff /tmp/0.jpg /tmp/0_bis.jpg


# --- Test parallel seeding ---
echo "== Testing parallel seeding with -p 4 =="
rm -rf /tmp/mc/global
mapcache_seed -c /tmp/mc/mapcache.xml -t global --force -z 0,2 -p 4
gdalinfo /tmp/mc/global/GoogleMapsCompatible/00/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 0/0/0"; gdalinfo /tmp/mc/global/GoogleMapsCompatible/00/000/000/000/000/000/000.jpg; /bin/false)
gdalinfo /tmp/mc/global/GoogleMapsCompatible/01/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 1/0/0"; gdalinfo /tmp/mc/global/GoogleMapsCompatible/01/000/000/000/000/000/000.jpg; /bin/false)
gdalinfo /tmp/mc/global/GoogleMapsCompatible/02/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 2/0/0"; gdalinfo /tmp/mc/global/GoogleMapsCompatible/02/000/000/000/000/000/000.jpg; /bin/false)
echo "OK: Parallel seeding with -p 4 successful"
rm -rf /tmp/mc/global

# --- Test parallel seeding with -p 1 ---
echo "== Testing parallel seeding with -p 1 =="
rm -rf /tmp/mc/global
mapcache_seed -c /tmp/mc/mapcache.xml -t global --force -z 0,2 -p 1
gdalinfo /tmp/mc/global/GoogleMapsCompatible/00/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 0/0/0"; exit 1)
gdalinfo /tmp/mc/global/GoogleMapsCompatible/01/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 1/0/0"; exit 1)
gdalinfo /tmp/mc/global/GoogleMapsCompatible/02/000/000/000/000/000/000.jpg | grep "Size is 256, 256" >/dev/null || (echo "Invalid image size for tile 2/0/0"; exit 1)
echo "OK: Parallel seeding with -p 1 successful"
rm -rf /tmp/mc/global

# --- Test graceful shutdown ---
echo "== Testing graceful shutdown of parallel seeder =="
rm -rf /tmp/mc_shutdown
mkdir -p /tmp/mc_shutdown
cp /tmp/mc/mapcache.xml /tmp/mc/mapcache_shutdown.xml
sed -i 's|<base>/tmp/mc</base>|<base>/tmp/mc_shutdown</base>|' /tmp/mc/mapcache_shutdown.xml
# Run seeder in the background, in its own process group
set -m
mapcache_seed -c /tmp/mc/mapcache_shutdown.xml -t global -z 0,8 -p 4 -q &
PID=$!
set +m
# Give it a moment to start seeding
sleep 2
PGID=$(ps -o pgid= $PID | xargs)
if [ -z "$PGID" ]; then
    echo "Could not get PGID of seeder process. Test cannot continue."
    exit 1
fi
# Send SIGINT to the process group
echo "Sending SIGINT to process group $PGID"
kill -INT -$PGID
# Wait for the process to terminate, with a timeout
if wait $PID 2>/dev/null; then
    echo "OK: Seeder terminated gracefully on SIGINT"
else
    # The process might have already exited and `wait` fails.
    # Let's check if it's still running.
    if ps -p $PID > /dev/null; then
        echo "Error: Seeder did not terminate gracefully"
        kill -9 -$PGID
        exit 1
    else
        echo "OK: Seeder terminated gracefully on SIGINT"
    fi
fi
rm -rf /tmp/mc_shutdown
