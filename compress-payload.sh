#!/bin/bash

# create a *.tar.xz archive
tar -C PAYLOAD/ -cvpf - . | xz -9 -c - > payload_new.tar.xz;

# check 
stat payload_new.tar.xz || exit 1;

# overwrite
mv payload_new.tar.xz payload.tar.xz;

echo "all done"
