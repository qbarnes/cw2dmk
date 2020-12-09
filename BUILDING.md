# Building `cw2dmk` Tools

## Building Natively on Linux

You will need several packages installed with the necessary build tools.

On Fedora and many RHEL-derivatives, run:
```
$ sudo dnf install -y util-linux groff-base git make gcc pciutils-devel
```

On Ubuntu and many Debian-derivatives, run:
```
$ sudo apt-get update
$ sudo apt-get install -y bsdmainutils groff-base git make gcc libpci-dev
```

To build for Linux, just run:
```
$ make
```

## Cross-building for MS-DOS Using Linux

You will need several packages installed with the necessary build tools.

On Fedora and many RHEL-derivatives, run:
```
$ sudo dnf install -y util-linux bzip2 groff-base git wget make
```

On Ubuntu and many Debian-derivatives, run:
```
$ sudo apt-get update
$ sudo apt-get install -y bsdmainutils groff-base git wget make
```

Install the latest DJGPP tools in a location to your liking.  This
example will use `/usr/local/djgpp/bin`.

Check `https://github.com/andrewwutw/build-djgpp/releases` for the
latest release.  At this time, it's `v3.1`.  To install its binaries
under `/usr/local/djgpp`, run:
```
$ wget https://github.com/andrewwutw/build-djgpp/releases/download/v3.1/djgpp-linux64-gcc1020.tar.bz2
$ sudo tar -C /usr/local -xf djgpp-linux64-gcc1020.tar.bz2
```

To cross-build using the DJGPP tools, from the repo's top directory, run:
```
$ make TARGET_OS=MSDOS CC=/usr/local/djgpp/bin/i586-pc-msdosdjgpp-gcc
```
