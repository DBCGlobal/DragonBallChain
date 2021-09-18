### Build Environment
* For now, support Linux-like 64bit systems, such as Ubuntu 14.x/Ubuntu 16.x/Ubuntu 18.x/Cent OS 7.x only.
* Following prerequisite libraries/tools must be installed prior to the build process.

1. Ubuntu 14.x/16.x/18.x/CentOS 7.x
```
$ sudo curl https://raw.githubusercontent.com/DragonBallChain/DragonBallChain/master/distros/linuxshell/prepare_prerequisites.sh|bash
```

### Build steps
1. Get the source code [DragonBallChain](https://github.com/DragonBallChain/DragonBallChain)
2. Detailed steps
> you will find the core executable under the directory `src/coind`.

```code
$ cd DragonBallChain
$ sh ./distros/linuxshell/linux.sh
$ sh ./autogen-coin-man.sh coin
$ make
```
