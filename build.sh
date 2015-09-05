#!/bin/bash
KDIR=/lib/modules/$(uname -r)/build
if [ "$1" == "-v" ]; then
        V=1
else
        V=0
fi
make -C $KDIR M=$(pwd) V=$V
