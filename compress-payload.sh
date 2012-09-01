#!/bin/sh
cd PAYLOAD
if [ -e Superuser.apk.xz ]; then
	rm Superuser.apk.xz
	rm su.xz
fi;
if [ -e payload.tar ]; then
	rm -f payload.tar
fi;
chmod 644 Superuser.apk
chmod 755 su
xz -zekv9 Superuser.apk
xz -zekv9 su

mv Superuser.apk.xz res/misc/payload/
mv su.xz res/misc/payload/

rm -f ../payload.tar
tar -cv res > payload.tar 
stat payload.tar
mv payload.tar ../
cd ..

