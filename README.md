# StopBath engine helper

A small program built from ART's own processing engine (ART by Alberto Griggio, itself a fork of RawTherapee), for
the StopBath photo app. StopBath starts it and talks to it; it develops photos the way ART does.

**Version 0.2 (step 2, now):** StopBath starts the helper once and sends it the same requests it would give ART-cli
(the same settings files, the same pictures out). The helper answers them without starting ART again, keeps the last
photos loaded, and reuses ART's first steps (decoding, demosaicing, the small picture ART starts from, the noise
measured for automatic noise reduction) while only later settings change, so a change shows sooner. Its pictures
are ART-cli's, pixel for pixel. Anything it doesn't do is left to ART-cli.

    stopbath-engine --version
    stopbath-engine --serve [--keep <n>]        (StopBath's way; requests on standard input)
    stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]   (a timing test)

`--serve` reads requests one at a time:

    run <id> <count>      then <count> lines, ART-cli's arguments one per line (-o/-O, -p, -d, -Y, -f, -j, -js, -t,
                          -b, -n, -c <one photo>)
    cancel <id>           stop it at its next step
    quit

and answers `@@SB ready <helper version> <ART version>` once, then `@@SB done <id> <exit code> <ms> <what was
reused> <message>` for each (exit code 99: not done by the helper - use ART-cli; 98: stopped). Other lines on
standard output are ART's own.

Built automatically on every upload (the Actions tab): the result, `stopbath-engine-windows-x64`, is on each
successful run's page. It is ART's own build of version 1.26.10 with the helper added, so it also holds ART and
ART-cli.

## Licence

GNU General Public License version 3 or later (see LICENSE), as ART. The source of ART used is ART's own
(https://github.com/artraweditor/ART, the release named in `.github/workflows/build.yml`); this repository holds the
helper's own source and how it is built. The helper's export pipeline is ART's own (`simpleprocess.cc`), copied
into `helper/stopbath-engine.cc` with the reuse and stopping added.
