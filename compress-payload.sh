#!/bin/sh
cd PAYLOAD
chmod 644 Superuser.apk
chmod 755 su
chmod 644 Extweaks.apk
xz -zekv9 Superuser.apk
xz -zekv9 su
xz -zekv9 Extweaks.apk

mv Superuser.apk.xz res/misc/payload/
mv su.xz res/misc/payload/
mv Extweaks.apk.xz res/misc/payload/

rm -f ../payload.tar
tar -cv res > payload.tar
stat payload.tar
mv payload.tar ../
cd ..
echo "all done"

