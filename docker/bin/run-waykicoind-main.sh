#!/bin/bash

cd /opt/docker-instances/waykicoind-main \
&& docker run --name waykicoind-main -p8920:8920 -p 6968:6968 \
       -v `pwd`/conf/DragonBallChain.conf:/root/.DragonBallChain/DragonBallChain.conf \
       -v `pwd`/data:/root/.DragonBallChain/main \
       -v `pwd`/bin:/opt/wicc/bin \
       -d wicc/waykicoind:v3.0
