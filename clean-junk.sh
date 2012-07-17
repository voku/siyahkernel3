#!/system/bin/sh
JUNK=`find -name *.rej`
for i in $JUNK
do
rm -f $i
done

JUNK2=`find -name *.orig`
for i in $JUNK2
do
rm -f $i
done

CLEANBKP () {
JUNK3=`find -name *.bkp`
for i in $JUNK3
do
rm -f $i
done
}
CLEANBKP

JUNK4=`find -name *.ko`
for i in $JUNK4
do
rm -f $i
done
