#!/bin/sh

cp .config .config.bkp;
make ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- mrproper;
cp .config.bkp .config;
make clean;
