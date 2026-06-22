#!/usr/bin/env python3
# bake_stars.py -- CSOZ star-field bake tool (clean-room).
#
# Reads the Yale Bright Star Catalogue (BSC5, VizieR V/50 "catalog") raw .dat,
# extracts ONLY numeric facts (J2000 RA/Dec, Vmag, B-V) -- facts are not
# copyrightable (Feist v. Rural) -- converts to world-space unit direction
# vectors, and emits a generated C array (csz_stars_catalog.inl) consumed by
# csz_stars.cpp.
#
# MW REWORK (2026-06-21): the procedural all-sky FILL (200k) and the dense
# galactic-BAND star population (210k) are GONE. The Milky Way + dense fine
# starfield are now supplied by a sampled real all-sky panorama (see
# csz_panorama.cpp + bake/bake_panorama.py); the live point-star layer renders
# ONLY the real BSC5 stars for the twinkle/scintillation shimmer, trimmed at
# runtime to the brightest few-hundred via the csz_pano_twinkle_maglimit cvar
# (default V<=3.8). So this tool now emits ONLY the real BSC5 set -- the catalog
# shrinks from 419096 to ~9096 stars. The galactic basis vectors are still emitted
# (the panorama's galactic->equirect mapping reuses them).
#
# COLOUR: the catalogue stores the per-star B-V colour INDEX (a numeric fact), NOT
# a baked RGB. The runtime shader maps B-V -> Teff (Ballesteros 2012) -> Planckian-
# locus RGB and applies a brightness-coupled saturation.
#
# No catalogue-provided code or copyrightable selection/arrangement is copied; we
# read only the numeric columns. CDS/VizieR acknowledgement is owed and is embedded
# in CREDITS-stars.md + the generated .inl header.
#
# Output (repo-relative, reproducible from a fresh checkout):
#   ../csz_stars_catalog.inl   (the parent geom/ dir)
#   ./bake_stats.json          (verification numbers, not fabricated)
import gzip, json, math, os

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG_GZ = os.path.join(HERE, "bsc5_catalog.gz")
# geom/bake/ -> geom/ : the generated catalogue sits next to csz_stars.cpp.
GEOM_DIR = os.path.normpath(os.path.join(HERE, ".."))
OUT_INL = os.path.join(GEOM_DIR, "csz_stars_catalog.inl")
OUT_STATS = os.path.join(HERE, "bake_stats.json")

DEG = math.pi / 180.0

# ---------------------------------------------------------------------------
# vector helpers
def eqvec(ra_deg, dec_deg):
    ra = ra_deg * DEG; dec = dec_deg * DEG
    return (math.cos(dec) * math.cos(ra),
            math.cos(dec) * math.sin(ra),
            math.sin(dec))

def norm(v):
    l = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) or 1.0
    return (v[0]/l, v[1]/l, v[2]/l)

def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])

def dot(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def scl(a, s): return (a[0]*s, a[1]*s, a[2]*s)

# ---------------------------------------------------------------------------
# equatorial(J2000) -> galactic rotation, built from the IAU pole + centre.
NGP = norm(eqvec(192.85948, 27.12825))     # north galactic pole (RA,Dec J2000)
GCN = eqvec(266.40499, -28.93617)          # galactic centre (l=0,b=0)
g_z = NGP
g_x = norm(sub(GCN, scl(g_z, dot(GCN, g_z))))   # centre, orthogonalised to pole
g_y = norm(cross(g_z, g_x))

def eq_to_gal(v):
    return (dot(v, g_x), dot(v, g_y), dot(v, g_z))   # (toward-centre, y, toward-pole)

# ---------------------------------------------------------------------------
# galactic frame -> world frame. The galactic plane (Milky Way) arcs high overhead.
# The three world basis vectors are emitted into the .inl so csz_panorama.cpp maps
# the sampled panorama into the EXACT same galactic frame as the baked star positions
# (one band, aligned to the live bright stars).
# SLAVED to kMwRawPole / kMwRawCenter in csz_stars.cpp (and the same constants in
# csz_panorama.cpp). If the RAW slant values change, update ALL of them + re-bake.
RAW_POLE   = (0.400, -0.708, 0.582)   # == kMwRawPole   in csz_stars.cpp / csz_panorama.cpp
RAW_CENTER = (0.022,  0.642, 0.766)   # == kMwRawCenter in csz_stars.cpp / csz_panorama.cpp
W_POLE   = norm(RAW_POLE)                                 # image of galactic +Z
W_CENTER = norm(sub(RAW_CENTER, scl(W_POLE, dot(RAW_CENTER, W_POLE))))  # image of galactic +X (centre)
W_Y      = norm(cross(W_POLE, W_CENTER))                  # image of galactic +Y

def gal_to_world(g):
    gx, gy, gz = g
    return (W_CENTER[0]*gx + W_Y[0]*gy + W_POLE[0]*gz,
            W_CENTER[1]*gx + W_Y[1]*gy + W_POLE[1]*gz,
            W_CENTER[2]*gx + W_Y[2]*gy + W_POLE[2]*gz)

def eq_to_world(v):
    return gal_to_world(eq_to_gal(v))

# ---------------------------------------------------------------------------
# B-V colour index -> approximate blackbody temperature (Ballesteros 2012; a
# published formula = fact). Used ONLY for verification stats below; the runtime
# shader recomputes this exact relation so the catalogue can stay RGB-free.
def bv_to_temp(bv):
    bv = max(-0.4, min(2.0, bv))
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))

# ---------------------------------------------------------------------------
# parse BSC5 (fixed-format, J2000 columns per VizieR V/50 ReadMe). Each star is
# kept as (dirX, dirY, dirZ, vmag, bv).
def parse_bsc5():
    stars = []
    skipped = 0
    with gzip.open(CATALOG_GZ, "rt", encoding="latin-1") as f:
        for line in f:
            if len(line) < 110:
                skipped += 1; continue
            try:
                rah = line[75:77]; ram = line[77:79]; ras = line[79:83]
                des = line[83];   ded = line[84:86]; dem = line[86:88]; dee = line[88:90]
                if not rah.strip() or not ded.strip():
                    skipped += 1; continue
                ra = 15.0 * (int(rah) + int(ram)/60.0 + float(ras)/3600.0)
                dec = int(ded) + int(dem)/60.0 + int(dee)/3600.0
                if des.strip() == "-":
                    dec = -dec
                vtxt = line[102:107].strip()
                vmag = float(vtxt) if vtxt else 6.5
                bvtxt = line[109:114].strip()
                bv = float(bvtxt) if bvtxt else 0.6
            except ValueError:
                skipped += 1; continue
            w = eq_to_world(eqvec(ra, dec))
            stars.append((w[0], w[1], w[2], vmag, bv))
    return stars, skipped

# ---------------------------------------------------------------------------
def main():
    real, skipped = parse_bsc5()
    # MW REWORK: the live point-star layer is the REAL BSC5 set ONLY. The dense fine
    # starfield + Milky Way band come from the sampled panorama; the runtime trims this
    # set to the brightest few-hundred (csz_pano_twinkle_maglimit, default V<=3.8) so
    # only the stars worth twinkling are drawn live (the rest live in the panorama).
    stars = real

    # ---- emit generated .inl
    hdr = (
        "// csz_stars_catalog.inl -- GENERATED by bake/bake_stars.py. DO NOT EDIT.\n"
        "//\n"
        "// Star POSITIONS / MAGNITUDES / COLOUR INDICES are numeric facts from the\n"
        "// Yale Bright Star Catalogue (BSC5), VizieR catalogue V/50 -- facts are not\n"
        "// copyrightable (Feist v. Rural). The required CDS/VizieR acknowledgement is\n"
        "// in CREDITS-stars.md.\n"
        "//\n"
        "// MW REWORK (2026-06-21): REAL BSC5 stars ONLY (the procedural 200k fill +\n"
        "// 210k galactic-band populations were removed -- the Milky Way + dense fine\n"
        "// starfield are now a sampled panorama, see csz_panorama.cpp). The live layer\n"
        "// is trimmed at runtime to the brightest few-hundred (csz_pano_twinkle_maglimit).\n"
        "//\n"
        "// Layout: kStarData = N x 5 floats {dirX,dirY,dirZ, bv, vmag} -- world unit\n"
        "// direction (Quake Z-up), B-V colour index, visual magnitude. Star RGB is\n"
        "// NOT baked: the runtime shader maps B-V -> Teff (Ballesteros) -> Planckian\n"
        "// locus and applies brightness-coupled saturation. The galactic basis (world\n"
        "// frame) lets the panorama's galactic->equirect mapping stay aligned.\n")
    lines = [hdr]
    lines.append("static const int kStarCount = %d;" % len(stars))
    lines.append("static const int kStarCountReal = %d;   // from BSC5 (the entire live set now)" % len(real))
    lines.append("static const float kStarGalPoleWorld[3]   = { %.8ff, %.8ff, %.8ff };" % W_POLE)
    lines.append("static const float kStarGalCenterWorld[3] = { %.8ff, %.8ff, %.8ff };" % W_CENTER)
    lines.append("static const float kStarGalYWorld[3]      = { %.8ff, %.8ff, %.8ff };" % W_Y)
    lines.append("static const float kStarData[] = {")
    buf = []
    for s in stars:
        # {dirX,dirY,dirZ, bv, vmag}
        buf.append("%.6ff,%.6ff,%.6ff,%.4ff,%.3ff," % (s[0], s[1], s[2], s[4], s[3]))
    # a few stars per line to keep the file from being one huge line
    per = 5
    for i in range(0, len(buf), per):
        lines.append("\t" + "".join(buf[i:i+per]))
    lines.append("};")
    with open(OUT_INL, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    # ---- verification stats (NOT fabricated; computed from the baked data)
    mags = sorted(s[3] for s in stars)
    def pct(p):
        i = max(0, min(len(mags)-1, int(p * (len(mags)-1))))
        return mags[i]
    bvs = sorted(s[4] for s in stars)
    def bvpct(p):
        i = max(0, min(len(bvs)-1, int(p * (len(bvs)-1))))
        return bvs[i]
    temps = [bv_to_temp(s[4]) for s in stars]
    # how many stars pass the default live-twinkle cutoff (a few hundred expected)
    n_bright_38 = sum(1 for s in stars if s[3] <= 3.8)
    n_bright_45 = sum(1 for s in stars if s[3] <= 4.5)
    stats = {
        "real_count": len(real), "total": len(stars),
        "skipped_lines": skipped,
        "floats_per_star": 5, "layout": "dirX,dirY,dirZ,bv,vmag",
        "vmag_min": mags[0], "vmag_max": mags[-1],
        "vmag_p50": pct(0.50), "vmag_p99": pct(0.99),
        "bv_min": bvs[0], "bv_max": bvs[-1],
        "bv_p50": bvpct(0.50), "bv_p05": bvpct(0.05), "bv_p95": bvpct(0.95),
        "teff_min": min(temps), "teff_max": max(temps),
        "live_twinkle_count_vmag_le_3p8": n_bright_38,
        "live_twinkle_count_vmag_le_4p5": n_bright_45,
        "gal_pole_world": W_POLE, "gal_center_world": W_CENTER, "gal_y_world": W_Y,
        "out_inl": OUT_INL, "out_inl_bytes": os.path.getsize(OUT_INL),
    }
    with open(OUT_STATS, "w", encoding="utf-8") as f:
        json.dump(stats, f, indent=2)
    print(json.dumps(stats, indent=2))

if __name__ == "__main__":
    main()
