#!/bin/sh
cd PAYLOAD

rm -f ../payload.ta*
tar -cv res > payload.tar
tar -cvJ --xz . > payload.tar.xz
stat payload.tar.xz
mv payload.tar.xz ../
cd ..
echo "all done"

