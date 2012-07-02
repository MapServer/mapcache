#!/bin/bash

ASTYLEOPTS="--style=kr --indent=spaces=2 -c --lineend=linux -S"
ASTYLEBIN=astyle

$ASTYLEBIN $ASTYLEOPTS -R '*.c' '*.h'

#find . -name '*.orig' -exec rm -f {} \;
