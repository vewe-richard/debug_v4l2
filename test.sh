#!/bin/bash
set -e
make
sudo insmod my_module2.ko
sleep 1
sudo rmmod my_module2
sudo dmesg -c
