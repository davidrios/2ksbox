# The bundled General MIDI bank

`TimGM6mb.sf2` is the bank the MPU-401's General MIDI synth plays when a
machine names none of its own (doc 20; the player finds it by its
`LIBSYNTH_SF2` rule). It is 5.7 MB, **GPL-2**, by Tim Brechbill (2004)
with later work by David Bolton (2010). MuseScore 0.9.6–1.3 shipped it,
and Debian packages it as `timgm6mb-soundfont`. This copy is byte for
byte the one in `timgm6mb-soundfont_1.3.orig.tar.gz`:

    sha256  c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854
    size    5969788

Why this one of the free banks (doc 20 §4):

- **Its licence is ours.** The author states the samples are his own or
  public domain / GPL. A bank of uncertain sample provenance is one to
  point the user at, not to put in a GPL package; the larger,
  better-sounding GeneralUser GS says its provenance is only "as good as
  the information the original sources gave".
- **It is small enough to ship** and complete: all 128 melodic programs
  and a drum kit in 5.7 MB, against 30 MB for GeneralUser GS and 140 MB
  for FluidR3, in a package of ~90 MB.

It is a default, not a verdict. The bank decides more of how a game's
MIDI music sounds than anything else, so the machine form takes any
`.sf2` and says so.
