# StopBath engine helper

A small program built from ART's own processing engine (ART by Alberto Griggio, itself a fork of RawTherapee), for
the StopBath photo app. StopBath starts it and talks to it; it develops photos the way ART does.

**Version 0.7 (now):** as 0.6, and each answer's `<what was reused>` says how ART made the picture:
`geo:full=WxH;photo=WxH;crop=XxY;scale=S;out=WxH` - the full-size picture for these settings (the crop's size, else
the photo's, as turned), the photo's own size, where the crop sits in it, the scale of the quick route and the size of
the picture made - so StopBath places zoomed-in parts from ART's own numbers. `--memory small|normal|large` sets how
much it keeps: small keeps two middle pictures and none after stage 1 and 2; normal (as 0.6) three and three, up to
12 MP; large four and four, up to 30 MP. Pictures are still ART-cli's, pixel for pixel.

**Version 0.6:** as 0.5, and the picture after ART's stage 1 (channel mixer, exposure, HSL, tone equalizer)
and after its stage 2 (sharpening, impulse noise, defringe, colour correction, smoothing) is kept as well, for each
size and crop of picture asked for (at most three), as ART's own editor does: a change of the tone curve, saturation,
local contrast and the other stage 3 steps starts after stage 2, a change in stage 2 after stage 1. (Not after stage
2 when a mask of a later step is linked to an earlier step's mask.) The answer's `<what was reused>` says `late:2` or
`late:1`. `-N` marks a picture made ahead (StopBath's next photos): a photo not kept goes into a place of its own (the last
one made ahead, with its middle picture for a film's second picture; asked for normally later, it moves in among the
photos kept), a photo kept keeps nothing new for it - so the photo being edited stays kept. Pictures after stage 1
and 2 aren't kept for a whole photo at full size above 12 MP (memory); one pair per size of picture. A `.gitattributes` keeps the files' line endings as they are. Pictures are
still ART-cli's, pixel for pixel.

**Version 0.5:** as 0.4, and `-Q` writes StopBath's quick picture: an uncompressed 8-bit TIFF with the
output's colour profile and no other metadata (ART's own saving writes the file, then reads and writes it all again
to add the metadata), the pixels as ART-cli's 8-bit TIFF. A picture made from the kept earlier steps with a crop
copies only the crop's part of them (the same pixels). Pictures are still ART-cli's, pixel for pixel.

**Version 0.4:** as 0.3, and each answer also says how long ART's steps took: ART's own per-step timing
(`ART_PROFILE`, switched on by the helper; two clock readings a step), with the helper's own steps (reading the
picture out, noise reduction, the lens corrections, the output colours...) named `sb.*`. The answer's `<what was
reused>` ends with `,steps:<name>=<ms>;<name>=<ms>...`, slowest first. The build uses the GitHub actions' newer
versions (Node.js 24). Pictures are still ART-cli's, pixel for pixel.

**Version 0.3:** as 0.2, and it also keeps the photo as it is before ART's colour and tone steps (read out of
the raw steps, in the working colours, denoised, its spots, dehaze and lens corrections done) for the last photo: a
change of exposure, colour or tone, or the second of a film's two pictures, starts from there - as ART's own editor
does. `-R` writes a picture as three planes of 32-bit numbers ("SBRF", width, height, 3, then red, green and blue),
the values of ART's 32-bit TIFF, which StopBath's film reads at once. Pictures are still ART-cli's, pixel for pixel.

**Version 0.2:** StopBath starts the helper once and sends it the same requests it would give ART-cli
(the same settings files, the same pictures out). The helper answers them without starting ART again, keeps the last
photos loaded, and reuses ART's first steps (decoding, demosaicing, the small picture ART starts from, the noise
measured for automatic noise reduction) while only later settings change, so a change shows sooner. Its pictures
are ART-cli's, pixel for pixel. Anything it doesn't do is left to ART-cli.

    stopbath-engine --version
    stopbath-engine --serve [--keep <n>] [--memory small|normal|large]   (StopBath's way; requests on standard input)
    stopbath-engine --render <photo> <out.jpg> [--profile <file.arp>]... [--fast] [--repeat <n>]   (a timing test)

`--serve` reads requests one at a time:

    run <id> <count>      then <count> lines, ART-cli's arguments one per line (-o/-O, -p, -d, -Y, -f, -j, -js, -t,
                          -b, -n, -R, -Q, -N, -c <one photo>)
    cancel <id>           stop it at its next step
    quit

and answers `@@SB ready <helper version> <ART version>` once, then `@@SB done <id> <exit code> <ms> <what was
reused> <message>` for each (exit code 99: not done by the helper - use ART-cli; 98: stopped). Other lines on
standard output are ART's own.

**The engine package (no ART install needed):** the folder holds a complete portable ART (ART, ART-cli, camera
data, colour profiles, lens data, ART's bundled profiles) beside the helper. StopBath starts the helper, ART-cli
and ART with `ART_SETTINGS` / `ART_CACHE` pointing at StopBath's own folders, so an installed ART is neither needed
nor touched. A tag (e.g. `engine-0.7`) publishes the package as a public release
(`stopbath-engine-windows-x64.zip` + `.sha256`), which StopBath downloads.

Built automatically on every upload (the Actions tab): the result, `stopbath-engine-windows-x64`, is on each
successful run's page. It is ART's own build of version 1.26.10 with the helper added, so it also holds ART and
ART-cli.

## Licence

GNU General Public License version 3 or later (see LICENSE), as ART. The source of ART used is ART's own
(https://github.com/artraweditor/ART, the release named in `.github/workflows/build.yml`); this repository holds the
helper's own source and how it is built. The helper's export pipeline is ART's own (`simpleprocess.cc`), copied
into `helper/stopbath-engine.cc` with the reuse and stopping added.
