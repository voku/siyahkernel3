#!/bin/sh

cp .config .config.bkp;
make mrproper;
cp .config.bkp .config;
make clean;

JUNK=`find . -name *.o`;
for i in $JUNK; do
	ls $i;
	rm -f $i;
done;
