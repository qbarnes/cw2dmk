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

To build native Linux binaries,
[clone the repo with ssh](#cloning-the-repo),
change into its top level directory, and run:
```
$ make
```

The Linux binaries are in the top level directory with the names
`cw2dmk`, `dmk2cw`, `jv2dmk`, `dmk2jv3`, and `cwtsthst`.

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

To cross-build MS-DOS binaries using the DJGPP tools,
[clone the repo with ssh](#cloning-the-repo),
change into its top level directory, and run:
```
$ make TARGET_OS=MSDOS CC=/usr/local/djgpp/bin/i586-pc-msdosdjgpp-gcc
```

The MS-DOS binaries are in the top level directory with the names
`cw2dmk.exe`, `dmk2cw.exe`, `jv2dmk.exe`, `dmk2jv3.exe`, `cwtsthst.exe`,
and `cwsdpmi.exe`.

## Cloning the Repo

This section provides common assistance with cloning the repo with
an ssh certificate for authenication with GitHub.

Using the ssh method of authentication is necessary for the way
submodules are referenced.

To clone the repo using an ssh certificate, run:
```
$ git clone --recursive git@github.com:qbarnes/cw2dmk
```

The `--recursive` option clones the source along with any submodules.

If you have trouble running the above command, see Github docs for help with
[Connecting to GitHub with SSH](https://docs.github.com/en/authentication/connecting-to-github-with-ssh).
