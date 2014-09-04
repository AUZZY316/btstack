#!/bin/sh
DIR=`dirname $0`
FILE=$DIR/../include/btstack/version.h
REVISION=`svnversion | sed "s/\([0-9]*\).*/\1/"`
MAJOR=0
MINOR=8
DATE=`date "+%Y-%m-%d_%H:%M:%S"`
printf "// BTstack - version.h\n" > $FILE
printf "// - generated by %s\n" $0>> $FILE
printf "// - at %s\n" $DATE >>  $FILE
printf "#define BTSTACK_MAJOR %u\n"             $MAJOR >> $FILE
printf "#define BTSTACK_MINOR %u\n"             $MINOR >> $FILE
printf "#define BTSTACK_REVISION %u\n"          $REVISION >> $FILE
printf "#define BTSTACK_VERSION \"%u.%u-%u\"\n" $MAJOR $MINOR $REVISION >> $FILE
printf "#define BTSTACK_DATE \"%s\""			$DATE >> $FILE
