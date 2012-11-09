#!/bin/sh
cd PAYLOAD
chmod 644 Superuser.apk
chmod 755 su
chmod 644 STweaks.apk
xz -zekv9 Superuser.apk
xz -zekv9 su
xz -zekv9 STweaks.apk

mv Superuser.apk.xz res/misc/payload/
mv su.xz res/misc/payload/
mv STweaks.apk.xz res/misc/payload/

rm -f ../payload.tar
tar -cv res > payload.tar
stat payload.tar
mv payload.tar ../
cd ..
md5sum PAYLOAD/STweaks.apk | awk '{print $1}' > ../initramfs3/res/stweaks_md5
chmod 644 ../initramfs3/res/stweaks_md5
echo "all done"

