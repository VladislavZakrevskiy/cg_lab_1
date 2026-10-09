#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
. ./env.sh
exec ./build-debug/cg-lab1
