#!/bin/bash
set -e
sudo dmesg -c
make
sudo insmod debug_v4l2.ko
sleep 1
sudo rmmod debug_v4l2
sudo dmesg -c
