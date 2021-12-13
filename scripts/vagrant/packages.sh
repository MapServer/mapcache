#!/bin/sh

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y cmake apache2
apt-get install -y libcurl4-openssl-dev apache2-dev
apt-get install -y libgdal-dev libfcgi-dev libpixman-1-dev gdal-bin libxml2-utils

