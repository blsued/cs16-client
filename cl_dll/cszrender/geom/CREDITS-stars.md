# CSOZ C4 Star Field — Asset Attribution & Provenance

This module (`csz_stars.cpp`, `csz_stars_math.h`, `csz_stars_shaders.inl`,
`csz_stars_catalog.inl`) renders the night-sky star field and Milky Way.

## Star positions / magnitudes / colours — Yale Bright Star Catalogue (BSC5)

The star **positions (J2000 RA/Dec), visual magnitudes, and B-V colour indices**
baked into `csz_stars_catalog.inl` are numeric facts extracted from the Yale
Bright Star Catalogue, 5th Revised Edition. Facts (and the facts within a
compilation) are not copyrightable (*Feist Publications v. Rural Telephone Service
Co.*, 499 U.S. 340 (1991)); only the CDS/VizieR **acknowledgement** is owed. No
catalogue-provided code or copyrightable selection/arrangement was copied — our
bake tool (in-repo at `cl_dll/cszrender/geom/bake/bake_stars.py`, with the raw
catalogue `cl_dll/cszrender/geom/bake/bsc5_catalog.gz` and the VizieR column
ReadMe `bake/bsc5_ReadMe.txt`) reads only the numeric columns from the raw `.gz`
sourced directly from VizieR/CDS. Re-running `python bake_stars.py` from that
directory regenerates `../csz_stars_catalog.inl` byte-for-byte (deterministic), so
the committed catalogue (the ~9,096 real BSC5 stars only) is fully reproducible from
a fresh checkout.

Required acknowledgement (embedded here per CDS policy):

```
Star data: Yale Bright Star Catalogue, 5th Revised Edition (Preliminary).
Hoffleit, D., & Warren, W. H., Jr., 1991, "The Bright Star Catalogue,
5th Revised Ed." (Bibcode 1991bsc..book.....H).
Distributed via CDS/VizieR catalogue V/50 (Bibcode 1995yCat.5050....0H).

This product has made use of the VizieR catalogue access tool, CDS,
Strasbourg, France (DOI: 10.26093/cds/vizier). The original description
of the VizieR service was published in 2000, A&AS 143, 23
(Ochsenbein, F., Bauer, P., & Marcout, J.).
```

Source URL of the raw catalogue used by the bake:
`http://cdsarc.u-strasbg.fr/ftp/cats/V/50/catalog.gz` (VizieR catalogue **V/50**;
see also <https://cdsarc.cds.unistra.fr/viz-bin/cat/V/50>).

## Live star set — real BSC5 only (MW rework)

The live twinkle layer renders only the ~9,096 real BSC5 stars, trimmed at draw time
to the brightest (vmag ≤ `csz_pano_twinkle_maglimit`, default 3.8) so the dense dim
starfield is supplied by the sampled panorama instead. The former procedural faint
fill (~200k) and the procedural galactic-band stars (~210k) were **DELETED** in the
MW rework; the catalogue now contains the real BSC5 set only (zero external data).

## Star colour & magnitude math — published formulas (facts)

- B-V → blackbody temperature: Ballesteros (2012), a published formula (fact).
- Blackbody → sRGB: a compact empirical fit (fact), re-typed clean-room.
- Magnitude → linear flux: Pogson's relation (fact).

No source code was copied; the relations are re-implemented from the published
mathematics in `csz_stars_math.h`.

## Milky Way band — sampled panorama (MW rework)

The procedural value-noise FBM Milky Way (`kStarsMwFs`) was **DELETED**. The band is
now a sampled equirectangular panorama drawn by `geom/csz_panorama.cpp`, baked offline
from NASA/GSFC SVS Deep Star Maps 2020 (Public Domain) + Gaia DR2 star data. Because a
real survey data asset is now used, its attribution lives in
`cl_dll/cszrender/CREDITS-sky-assets.md` (the "Milky Way all-sky panorama" entry) — it
is a separately-licensed runtime data asset, not part of this GPL source.

## License of our new files

All new C4 source files: **GPL-2.0-or-later** with the Half-Life engine linking
exception (matching the rest of cszrender). The BSC5 acknowledgement above is an
additive credit (no GPL conflict); it is not a source-license restriction.
