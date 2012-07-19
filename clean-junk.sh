#!/bin/sh

JUNK=`find . -name *.rej`;
for i in $JUNK; do
	ls $i;
	rm -f $i
done;

JUNK=`find . -name *.orig`;
for i in $JUNK; do
	ls $i;
	rm -f $i;
done;

JUNK=`find . -name *.bkp`;
for i in $JUNK; do
	ls $i;
	rm -f $i;
done;

JUNK=`find . -name *.ko`;
for i in $JUNK; do
	ls $i;
	rm -f $i;
done;

JUNK=`find . -name *.org`;
for i in $JUNK; do
        ls $i;
        rm -f $i;
done;
