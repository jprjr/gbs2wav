# gbs2wav

gbs2wav is a program to convert a given GBS file into
multiple WAV files, one per track, with embedded ID3 tags.

It uses the [SameBoy](https://github.com/LIJI32/SameBoy) core
for emulating the Game Boy.

## Usage

```
gbs2wav /path/to/file.gbs /path/to/file.m3u
```

This will read in a GBS file, and an optional M3U file. If
no M3U file is given, all songs default to 3 minutes with
a 10-second fade. There's no silence or end-of-song detection,
I primarily use this with M3U playlists that contain track
titles, song lengths, etc.

## Building

Just run `make`, this should build the `gbs2wav` program. There's
no external dependencies.

## LICENSE

MIT (see `LICENSE`).

The Sameboy sources retain their original
licensing (also `MIT`).
