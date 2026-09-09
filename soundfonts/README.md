# The bundled General MIDI bank

`TimGM6mb.sf2` — 5.7 MB, **GPL-2**, by Tim Brechbill (2004), with later
work by David Bolton (2010). It is the bank MuseScore 0.9.6–1.3 shipped,
and Debian packages it as `timgm6mb-soundfont`; this copy is byte for
byte the `timgm6mb-soundfont_1.3.orig.tar.gz` one:

    sha256  c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854
    size    5969788

Why this one, of the many free banks (doc 20 §4):

* **Its licence is ours.** GPL-2, and the author states the samples are
  either his own or public domain / GPL. A bank whose sample provenance
  is "as good as the information the original sources gave" — which is
  what the larger and better-sounding GeneralUser GS says of itself —
  is a thing to point the user *at*, not to put inside a GPL package.
* **It is small enough to ship.** 5.7 MB against 30 MB for GeneralUser
  GS and 140 MB for FluidR3, in a package that is otherwise ~90 MB, and
  it is a complete General MIDI set: all 128 melodic programs and a
  drum kit.

It is a *default*, not a verdict: the machine form takes any `.sf2`, and
a user with a favourite bank should use it — the difference between banks
is the single biggest thing about how a game's MIDI music sounds. The
wizard says so.
