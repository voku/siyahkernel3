#!/bin/bash

# create a *.tar.xz archive
tar -C RECOVERY-PAYLOAD/ -cvpf - . | xz -9 -c - > recovery_new.tar.xz;

# check 
stat recovery_new.tar.xz || exit 1;

# overwrite
mv recovery_new.tar.xz recovery.tar.xz;

echo "all done"

