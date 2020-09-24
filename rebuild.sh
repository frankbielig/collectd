#!/bin/sh

SELF=$(cd $(dirname "$0"); pwd)/$(basename "$0")
BASE_DIR=$(dirname "$SELF")
INSTALL_DIR="$BASE_DIR/install"

$BASE_DIR/clean.sh
$BASE_DIR/build.sh


./configure \
  --prefix="$INSTALL_DIR" \
  --with-perl-bindings="INSTALL_BASE=$INSTALL_DIR" \
  --with-libdlt="$HOME/work/github/GENIVI/dlt-daemon/install" \
  --disable-perl
  
  
make -j 6
make install

