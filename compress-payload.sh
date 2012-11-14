#!/bin/sh
cd PAYLOAD

rm -f ../payload.tar
tar -cv res > payload.tar
stat payload.tar
mv payload.tar ../
cd ..
echo "all done"

