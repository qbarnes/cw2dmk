#! /usr/bin/env python3
#
# Convert a cw2dmk level 5-7 logfile to a level 4 logfile equivalent.
#
# Command will read from standard input and write to standard output
# or from a list of log file names arguments.  If file names are
# provided, the output file names will have "-l4" suffixed to the
# basename of the file.
#

import sys
import os
import typing
import argparse
import re

def cw2dmk_log_filter(infile: typing.TextIO, outfile: typing.TextIO):
    """
    Read a cw2dmk generated log file from 'infile'.  Remove artifacts
    of a level 5 or higher log making it appear as as if it were a
    level 4 log.  Write the result to 'outfile'.
    """
    re1str  = r'([0-9]+[sml] )+'
    re1str += r'|(<[0-9a-f]+> )+'
    re1str += r'|\([+-][0-9]+\)'
    re1str += r'|\?'
    re1 = re.compile(re1str)

    for line in infile:
        # First, remove \r's at the end of the line.  The log might
        # have been generated on an OS with \r\n EOLs.  If the line
        # left after stripping a \r is more than just a \n, scan it
        # to remove all of the patterns in re1.  If the line ends
        # with an "ID CRC] " string, we don't want to output its \n,
        # so remove it.  If the only thing left of the line after
        # all pattern substitutions is just a \n, don't output it.

        if line.endswith('\r\n'):
            line = line[:-2] + '\n'
        if line != '\n':
            line = re1.sub('', line)
            if line.endswith('ID CRC] \n'):
                line = line[:-1]
            if line == '\n':
                continue

        outfile.write(line)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('logfile', nargs='*')
    args = parser.parse_args()

    if args.logfile:
        for ifn in args.logfile:
            (root, ext) = os.path.splitext(ifn)
            ofn = root + '-l4' + ext

            with open(ifn, 'r') as ifo, open(ofn, 'x') as ofo:
                cw2dmk_log_filter(ifo, ofo)
    else:
        cw2dmk_log_filter(sys.stdin, sys.stdout)

    return 0

if __name__ == '__main__':
    sys.exit(main())
