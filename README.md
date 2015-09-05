# Disk Latency Tracing

Disk latency tracing tracks all IO latencies and enables the following:
* See all IOs to all devices and watch their CDBs, sense buffers and latencies
* Monitor in the background the above and only dump on error (timeout, sense key)

The intent of all of this is to allow to debug rare IO related issues and to be
able to understand disk behavior by the latency of the IOs. The resulting
understanding can drive improvements in the software to avoid issues stemming
from misaligned drive behavior from software expectations.

## Status

This is a work in progress and has not reached a state of usefulness yet.

## How to build

### Debian and Ubuntu

#### Build

1. apt-get install linux-headers-amd64
2. ./build.sh

#### Test

1. apt-get install virtualbox virtualbox-dkms
2. Create a VM with a Debian netinst CD and use the provision/debian-preseed.cfg to create it (use "auto url=http://webserver/debian-preseed.cfg" as the boot command instead of manually installing it)
3. Copy and run the setup-dev.sh script to the image, it will setup kdump, graphics in a large screen mode and some tools I like to have
4. Set a shared folder in virtualbox named disklatency and pointing to this git repo
5. Use the build.sh script to create a kernel module for your virtual machine image

### Other Linux

Follow the above but adapt them to your OS, let me know the steps so I can update it for the benefit of others.

## Author

Baruch Even <baruch@ev-en.org>
