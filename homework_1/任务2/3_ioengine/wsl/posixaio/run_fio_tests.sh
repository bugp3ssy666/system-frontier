#!/bin/bash

echo "创建文件"
fio --name=init \
    --filename=testfile \
    --size=1G \
    --rw=write \
    --bs=1M \
    --numjobs=1 \
    --ioengine=libaio \
    --direct=1

echo "Test 1: 顺序读"
fio --name=seqread \
    --filename=testfile \
    --rw=read \
    --bs=4k \
    --size=1G \
    --ioengine=libaio \
    --iodepth=1 \
    --numjobs=1 \
    --direct=1 \
    --output=seqread.txt

echo "Test 2: 顺序写"
fio --name=seqwrite \
    --filename=testfile \
    --rw=write \
    --bs=4k \
    --size=1G \
    --ioengine=libaio \
    --iodepth=1 \
    --numjobs=1 \
    --direct=1 \
    --output=seqwrite.txt

echo "Test 3: 随机读"
fio --name=randread \
    --filename=testfile \
    --rw=randread \
    --bs=4k \
    --size=1G \
    --ioengine=libaio \
    --iodepth=1 \
    --numjobs=1 \
    --direct=1 \
    --output=randread.txt

echo "Test 4: 随机写"
fio --name=randwrite \
    --filename=testfile \
    --rw=randwrite \
    --bs=4k \
    --size=1G \
    --ioengine=libaio \
    --iodepth=1 \
    --numjobs=1 \
    --direct=1 \
    --output=randwrite.txt
