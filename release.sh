#!/bin/bash

ms_version=$1

echo ""
echo "# Release steps for mapcache version $ms_version (mapcache-$ms_version.tar.gz)"
echo ""

ms_version_suffix=`echo $ms_version | cut -s -d- -f2`
if [ -z $ms_version_suffix ]; then
  ms_version_num=$ms_version
else
  ms_version_num=`echo $ms_version | cut -s -d- -f1`
fi

ms_version_major=`echo $ms_version_num | cut -d. -f1`
ms_version_minor=`echo $ms_version_num | cut -d. -f2`
ms_version_revision=`echo $ms_version_num | cut -d. -f3`


tagname=rel-$ms_version_major-$ms_version_minor-$ms_version_revision
if [ ! -z $ms_version_suffix ]; then
  tagname=$tagname-$ms_version_suffix
fi

echo "#"
echo "# Make sure:"
echo "# - you are on branch-$ms_version_major-$ms_version_minor"
echo "# - you have edited MIGRATION_GUIDE.txt with changes related to this release"
echo "#"
echo ""

echo "sed -i '/set (MAPCACHE_VERSION_MAJOR/c\set (MAPCACHE_VERSION_MAJOR $ms_version_major)' CMakeLists.txt"
echo "sed -i '/set (MAPCACHE_VERSION_MINOR/c\set (MAPCACHE_VERSION_MINOR $ms_version_minor)' CMakeLists.txt"
echo "sed -i '/set (MAPCACHE_VERSION_REVISION/c\set (MAPCACHE_VERSION_REVISION $ms_version_revision)' CMakeLists.txt"
if [ ! -z $ms_version_suffix ]; then
  echo "sed -i '/set (MAPCACHE_VERSION_SUFFIX/c\set (MAPCACHE_VERSION_SUFFIX \"-$ms_version_suffix\")' CMakeLists.txt"
else
  echo "sed -i '/set (MAPCACHE_VERSION_SUFFIX/c\set (MAPCACHE_VERSION_SUFFIX \"\")' CMakeLists.txt"
fi

echo "git add CMakeLists.txt"
echo "git commit -m \"update for $ms_version release\""
echo "git tag -a $tagname -m \"Create $ms_version tag\""
echo "git push origin branch-$ms_version_major-$ms_version_minor --tags"
echo "git archive --format=tar.gz --prefix=mapcache-$ms_version/ $tagname >/tmp/mapcache-$ms_version.tar.gz"
echo "scp /tmp/mapcache-$ms_version.tar.gz download.osgeo.org:/osgeo/download/mapserver"

echo ""
echo "#"
echo "# Optionally update doc site, these commands need tweaking before being ran"
echo "#"

echo "/path/to/mapcache/release-notes.sh rel-previous-tag..$tagname >> /path/to/docs/en/development/changelog/changelog-$ms_version_major-$ms_version_minor.txt"

