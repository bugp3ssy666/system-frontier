@echo off

REM 设置测试文件路径
set FILE=testfile.dat

echo.
echo [1/5] 创建测试文件
fio.exe --name=init ^
    --filename=%FILE% ^
    --size=1G ^
    --rw=write ^
    --bs=1M ^
    --direct=1 ^
    --numjobs=1

echo.
echo [2/5] 顺序读测试 (Sequential Read)
fio.exe --name=seqread ^
    --filename=%FILE% ^
    --rw=read ^
    --bs=4k ^
    --size=1G ^
    --ioengine=windowsaio ^
    --iodepth=16 ^
    --numjobs=1 ^
    --direct=1 ^
    --output=seqread.txt

echo.
echo [3/5] 顺序写测试 (Sequential Write)
fio.exe --name=seqwrite ^
    --filename=%FILE% ^
    --rw=write ^
    --bs=4k ^
    --size=1G ^
    --ioengine=windowsaio ^
    --iodepth=16 ^
    --numjobs=1 ^
    --direct=1 ^
    --output=seqwrite.txt

echo.
echo [4/5] 随机读测试 (Random Read)
fio.exe --name=randread ^
    --filename=%FILE% ^
    --rw=randread ^
    --bs=4k ^
    --size=1G ^
    --ioengine=windowsaio ^
    --iodepth=16 ^
    --numjobs=1 ^
    --direct=1 ^
    --output=randread.txt

echo.
echo [5/5] 随机写测试 (Random Write)
fio.exe --name=randwrite ^
    --filename=%FILE% ^
    --rw=randwrite ^
    --bs=4k ^
    --size=1G ^
    --ioengine=windowsaio ^
    --iodepth=16 ^
    --numjobs=1 ^
    --direct=1 ^
    --output=randwrite.txt

echo.
echo 测试完成！结果保存为同目录下txt文件

pause