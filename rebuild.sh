#!/bin/sh

SELF=$(cd $(dirname "$0"); pwd)/$(basename "$0")
BASE_DIR=$(dirname "$SELF")
INSTALL_DIR="$BASE_DIR/install"

rm -rf $INSTALL_DIR
$BASE_DIR/clean.sh
$BASE_DIR/build.sh

CFLAGS="-g -O0" \
./configure \
  --prefix="$INSTALL_DIR" \
  --with-perl-bindings="INSTALL_BASE=$INSTALL_DIR" \
  --with-libdlt="$HOME/work/github/GENIVI/dlt-daemon/install" \
  --disable-perl \
  --disable-client-library \
  --enable-debug

make -j 6
make check
make install

