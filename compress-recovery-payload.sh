#!/bin/sh
chown root:root RECOVERY-PAYLOAD -R
cd RECOVERY-PAYLOAD
chmod 777 sbin -R
chmod 644 res -R
rm -f ../recovery.tar.xz
tar -cvJ --xz . > ../recovery.tar.xz
stat ../recovery.tar.xz
cd ..

