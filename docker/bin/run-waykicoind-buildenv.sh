#!/bin/bash

# Usage: launch a container to build DragonBallChain source code from github repo
#
NET=$1
WORK_DIR=$2
SRC_DIR=$3

[ -z "$NET" ] && NET=testnet
 
docker run --name build-waykicoind \
       -v $WORK_DIR/conf/DragonBallChain.conf:/root/.DragonBallChain/DragonBallChain.conf \
       -v $WORK_DIR/data:/root/.DragonBallChain/$NET \
       -v $WORK_DIR/bin:/opt/wicc/bin \
       -v $SRC_DIR:/opt/wicc/src:rw \
       -it wicc/waykicoind bash
