#!/bin/sh

NUMTHREADS=2 # we have 2 cpus configured
export NUMTHREADS

cd /vagrant

mkdir -p build_vagrant
cd build_vagrant
cmake ..

make -j $NUMTHREADS
make install

ldconfig
