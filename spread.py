#!/usr/bin/env python3

import sys, os, os.path, gzip, random

if len(sys.argv) != 3:
    print("Usage: %s [/var/log/]file.log n_files" % sys.argv[0])
    print("Read all logs for a program, decompressing them if necessary,")
    print("and then write them out into n files in the current directory with lines randomly interleaved.")
    sys.exit(1)

logdir, logname = os.path.split(sys.argv[1])
n = int(sys.argv[2])
if logdir == '':
    logdir = '/var/log/'

lines = []
for logfile in os.listdir(logdir):
    if not logfile.startswith(logname):
        continue
    logfile = os.path.join(logdir, logfile)
    print('reading', logfile)
    if logfile.endswith('.gz'):
        file = gzip.open(logfile)
    else:
        file = open(logfile, 'rb')
    prevline = None
    for line in file:
        lines.append(line)
        continue
        if prevline is not None and line < prevline:
            print('note: lines in %s are not in order ("%s" > "%s")' % (logfile, prevline, line))
            prevline = None
        else:
            prevline = line
    file.close()
lines.sort()

outfiles = []
outsuffixes = 'abcdefghijklmnopqrstuvwxyz'
for i in range(n):
    outname = logname + '.' + outsuffixes[i]
    outfiles.append([outname, open(outname, 'wb'), 0])

random.seed()
for line in lines:
    file = outfiles[random.randrange(n)]
    file[1].write(line)
    file[2] += 1

print('\ndivided %d lines across %d files:' % (len(lines), n))
for name, file, lines in outfiles:
    print('%s: %d lines' % (name, lines))
    file.close()
