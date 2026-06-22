# CSOZ sky renderer — asset attributions (CREDITS / ATTRIBUTIONS manifest)

These are SEPARATE licensed/public-domain data assets loaded at runtime. They are
NOT compiled into the GPL-2.0-or-later renderer source; keep their attributions
here (and in any redistributed build's NOTICE), not as a source-license header on
the binary. See `D:\csoz-sky-build\provenance\provenance-draft.md` for the full
rights analysis.

## Moon surface texture (C3 — `gfx/csz/moon_color.png`)

Public domain (US Government work), used as the lunar disc albedo by
`geom/csz_sunmoon.cpp`.

```
Moon texture: NASA's Scientific Visualization Studio (CGI Moon Kit).
Ernie Wright (USRA), Noah Petro (NASA/GSFC). LROC WAC color mosaic
courtesy NASA/GSFC/Arizona State University; elevation from LRO LOLA.
```

NASA no-endorsement notice (verbatim NASA policy — required):

```
NASA material may not be used to state or imply the endorsement by NASA
or by any NASA employee of a commercial product, service, or activity,
or used in any manner that might mislead.
```

Compact embeddable form: *"Moon imagery is public domain, courtesy NASA's
Scientific Visualization Studio. NASA does not endorse CSOZ or any associated
product or service."*

Source: <https://svs.gsfc.nasa.gov/4720> ; rights:
<https://www.nasa.gov/nasa-brand-center/images-and-media/>

Processing: reprojected to an equirectangular lon/lat color map and downsized for
runtime; pixel data only, no NASA insignia/logo, no identifiable persons.

## Milky Way all-sky panorama (MW-rework — `gfx/csz/mw_panorama.bin` [CSZP])

Public domain (NASA), credit requested. The deep-space backdrop sampled by
`geom/csz_panorama.cpp`. Baked offline (see `geom/bake/bake_panorama.py`) from the
NASA/GSFC SVS **Deep Star Maps 2020** GALACTIC-coordinate variant: the starless
Milky Way band (`milkyway_2020_8k_gal.exr`) composited with the dim starfield of the
starmap (`starmap_2020_8k_gal.exr`) with the brightest point sources removed (those
are rendered live by the BSC5 twinkle layer). The raw EXR sources are **not** kept in
repo (they are large, ~272 MB, and re-downloadable Public Domain bake *inputs*); only
the runtime asset `out/mw_panorama.bin` is committed. The bake is reproducible by
re-downloading the two galactic-coordinate (`_gal`) variants directly from NASA SVS:

```
milkyway_2020_8k_gal.exr (starless MW band, 8192x4096, OpenEXR 32-bit float):
  https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/milkyway_2020_8k_gal.exr
starmap_2020_8k_gal.exr  (dense starfield + band, 8192x4096, OpenEXR 32-bit float):
  https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/starmap_2020_8k_gal.exr
```

Place both under `geom/bake/panorama_src/` and run `python bake_panorama.py` from
`geom/bake/` to regenerate `out/mw_panorama.bin` deterministically.

```
Sky map: NASA/Goddard Space Flight Center Scientific Visualization Studio
(Deep Star Maps 2020, SVS 4851, galactic-coordinate variant). Public domain (NASA),
credit requested. Star data: Gaia DR2 — ESA/Gaia/DPAC. Constellation figures based
on those developed for the IAU by Alan MacRobert of Sky & Telescope.
```

NASA no-endorsement notice (verbatim NASA policy — required):

```
NASA material may not be used to state or imply the endorsement by NASA
or by any NASA employee of a commercial product, service, or activity,
or used in any manner that might mislead.
```

Compact embeddable form: *"Milky Way panorama derived from NASA/GSFC SVS Deep Star
Maps 2020 (public domain); star data Gaia DR2 (ESA/Gaia/DPAC). NASA does not endorse
CSOZ or any associated product or service."*

Source: <https://svs.gsfc.nasa.gov/4851> ; rights:
<https://www.nasa.gov/nasa-brand-center/images-and-media/>

Processing: starless-band + bright-removed dim-starfield composite, color-graded
toward a real astrophotography look (saturation lift, localized warm/H-alpha core,
cool arms, deepened dust lanes), tonemapped HDR→RGB8 with a black-point background
subtraction, resampled to 4096×2048, offline 13-level mip pyramid, gamma-2.2 display
encode, packed to the raw CSZP container. No sky color and no sun/moon are baked in
(atmosphere owns the navy; `csz_sunmoon` owns the bodies). Pixel data only.
