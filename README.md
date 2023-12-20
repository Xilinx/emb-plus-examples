# Prerequisites
```
$ source /proj/xbuilds/2023.1_daily_latest/installs/lin64/Vivado/2023.1/settings64.sh
$ source /proj/xbuilds/2023.1_released/xbb/xrt/packages/setenv.sh
$ install opencv on x86 host machine

```
# Generating xclbin
```
$ cd root directory of the repo
$ make all
```

# Compiling F2d application
```
$ cd app
$ make host TARGET=hw
```
# Test application on V70 target
```
$ ssh xsjvivekana50
$ source above pre-req tools
$ # xbutil program -d 0000:17:00.1 -u binary_container_1.xclbin
$ cp _x/link/int/binary_container_1.xclbin app/filter2d_accel.xclbin
$ ./filter2d.exe ../test_images/flower_1920x1080.yuv
```
# License
(C) Copyright 2024, Advanced Micro Devices Inc.\
SPDX-License-Identifier: Apache-2.0
