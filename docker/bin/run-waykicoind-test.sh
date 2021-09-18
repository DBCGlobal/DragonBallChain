#!/bin/bash

cd /opt/docker-instances/waykicoind-test \
&& docker run --name waykicoind-test -p18920:18920 -p 6967:6968 \
       -v `pwd`/conf/DragonBallChain.conf:/root/.DragonBallChain/DragonBallChain.conf \
       -v `pwd`/data:/root/.DragonBallChain/testnet \
       -v `pwd`/bin:/opt/wicc/bin \
       -d wicc/waykicoind