#!/bin/sh

echo '* Running libtorrent-rasterbar autotool.sh...'
cd third_party/libtorrent-rasterbar
./autotool.sh
cd ../..
echo

echo '* Runing libtoolize...'
libtoolize --install --copy --force --automake

echo '* Running aclocal...'
aclocal

echo '* Runing autoconf...'
autoconf --force -Wall

echo '* Running autoheader...'
autoheader

#echo '* Running automake...'
automake --add-missing --copy --force-missing

