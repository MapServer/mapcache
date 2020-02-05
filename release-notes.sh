#!/bin/bash

# Run from a MapCache git clone, ./release-notes.sh tag1..tag2 , e.g. for changes
# from 1.8.0 to 1.8.1, run
# ./release-notes.sh rel-1-8-0..rel-1-8-1
# Output from this script can be appended to the changelogs after having been
# manually filtered of irrelevant commits

tags=$1
if test -z $tags; then
   echo "usage: $0 startcommit..endcommit"
   exit
fi

SED=sed
if [ "$(uname)" = "Darwin" ]; then
  SED=gsed
fi

git --no-pager  log --no-merges  --pretty=format:'* %s (%an) : `%h <https://github.com/mapserver/mapcache/commit/%H>`__' $tags | $SED  's!#\([0-9]\+\)! `#\1 <https://github.com/mapserver/mapcache/issues/\1>`__ !g'
