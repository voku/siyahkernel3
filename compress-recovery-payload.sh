#!/bin/sh
chown root:root RECOVERY-PAYLOAD -R
cd RECOVERY-PAYLOAD
rm -f ../recovery.tar.xz
tar -cvJ --xz . > ../recovery.tar.xz
stat ../recovery.tar.xz
cd ..

