#!/bin/bash
sudo rmmod my_debug_v4l2
sudo dmesg -c > /dev/null 2>&1
make
sudo insmod my_debug_v4l2.ko
sleep 1
sudo dmesg -c
