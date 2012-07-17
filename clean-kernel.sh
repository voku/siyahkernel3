#!/bin/bash

cp .config .config.bkp;
make mrproper;
cp .config.bkp .config;
make clean;
