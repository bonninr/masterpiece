#!/usr/bin/env bash
# Set the release version in the one place that defines it, and in the one
# place that repeats it.
#
#   cmake/set-version.sh 0.4.2
#
# CMakeLists.txt is the source of truth: the binary, the installer, the
# package and the download file names all take their version from it. The
# README has to repeat it, because every published file now carries the
# version in its name and the download badges link straight at a file. Run
# this on the release branch, before tagging, so the badges point at the
# files the tag is about to publish.
set -e

VER="${1:?usage: $0 <version>   e.g. $0 0.4.2}"
case "$VER" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "$0: version must look like 1.2.3, got '$VER'" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/.."

sed -i -E "s/^(project\(Masterpiece VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1$VER/" CMakeLists.txt

# The download links carry the version twice: once as the tag the release
# lives under, and once in the file name itself.
sed -i -E "s|releases/download/v[0-9]+\.[0-9]+\.[0-9]+/|releases/download/v$VER/|g" README.md
sed -i -E "s|masterpiece-[0-9]+\.[0-9]+\.[0-9]+-|masterpiece-$VER-|g" README.md

grep -n '^project(Masterpiece' CMakeLists.txt
grep -c "masterpiece-$VER-" README.md | sed 's/^/README references: /'
