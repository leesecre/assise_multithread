#! /bin/bash

rm -rf a.strace
./run.sh ./filebench.mlfs -f varmail_mlfs.f
#./run.sh strace -ff ./filebench.mlfs -f varmail_mlfs.f 2> a.strace


#set environment LD_PRELOAD ../../libfs/lib/jemalloc-4.5.0/lib/libjemalloc.so.2
#set environment LD_LIBRARY_PATH ../../libfs/lib/nvml/src/nondebug/:../../libfs/build/
:../../libfs/src/storage/spdk/
#set environment DEV_ID 4

#set follow-fork-mode child
#gdb --args ./filebench.mlfs -f varmail_mlfs.f