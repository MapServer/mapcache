#!/bin/sh

# Project:  MapCache
# Purpose:  MapCache tests
# Author:   Even Rouault
#
#*****************************************************************************
# Copyright (c) 2017 Regents of the University of Minnesota.
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

mkdir -p /tmp/mc
sudo chmod -R a+rw /tmp/mc

cp data/mapcache.xml data/world.tif /tmp/mc

sudo cp data/mapcache.load data/mapcache.conf /etc/apache2/mods-available
if [ ! -L /etc/apache2/mods-enabled/mapcache.load ]
then
  sudo ln -s ../mods-available/mapcache.load /etc/apache2/mods-enabled
fi
if [ ! -L /etc/apache2/mods-enabled/mapcache.conf ]
then
  sudo ln -s ../mods-available/mapcache.conf /etc/apache2/mods-enabled
fi

sudo apache2ctl -k stop
sudo apache2ctl -k start
