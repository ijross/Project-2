#!/bin/sh
cp schedproc.h /usr/src/servers/sched/
cp schedule.c /usr/src/servers/sched/
cp pm/schedule.c /usr/src/servers/pm/
cp config.h /usr/src/include/minix/

cc cpu.c -o cpu
cc io.c -o io

cd /usr/src/tools
make install

