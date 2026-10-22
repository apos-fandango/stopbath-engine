# StopBath engine helper

A small program built from ART's own processing engine (ART by Alberto Griggio, itself a fork of RawTherapee), for
the StopBath photo app. StopBath starts it and talks to it; it develops photos the way ART does.

**Step 1 (now):** proves ART's engine builds on GitHub's Windows machines and times one photo:

    stopbath-engine --version
    stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]

Built automatically on every upload (the Actions tab): the result, `stopbath-engine-windows-x64`, is on each
successful run's page. It is ART's own build of version 1.26.10 with the helper added, so it also holds ART and
ART-cli.

## Licence

GNU General Public License version 3 or later (see LICENSE), as ART. The source of ART used is ART's own
(https://github.com/artraweditor/ART, the release named in `.github/workflows/build.yml`); this repository holds the
helper's own source and how it is built.
