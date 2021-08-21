#!/bin/sh

sed -i 's#deb http://us.archive.ubuntu.com/ubuntu/#deb mirror://mirrors.ubuntu.com/mirrors.txt#' /etc/apt/sources.list

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y python-software-properties
add-apt-repository -y ppa:ubuntugis/ubuntugis-unstable
apt-get update
apt-get -y upgrade

# install packages we need
apt-get install -q -y build-essential pkg-config cmake libgeos-dev rake vim \
    bison flex libgdal1-dev libproj-dev libpng12-dev libjpeg-dev libfcgi-dev \
    libcurl4-gnutls-dev apache2-prefork-dev libtiff4-dev libpixman-1-dev \
    libsqlite3-dev libmemcached-dev
