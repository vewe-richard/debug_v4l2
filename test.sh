#!/bin/bash
set -e
sudo rmmod debug_v4l2
sudo dmesg -c > /dev/null 2>&1
make
sudo insmod debug_v4l2.ko
sleep 1
sudo dmesg -c
