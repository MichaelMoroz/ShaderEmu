# A 12 oz drink can (211 x 413, a 202 lid): its outline, shared by its model (world/toys.py's
# soda_can) and its printed picture (world/textures.py's soda_cans). From the makers' sheets: the
# body 66.0 mm across and 122.2 mm high (Crown's 12 oz standard), the neck 52.4 mm (2.063 in, American
# Canning's plug diameter), the base 48.3 mm (1.900 in), a 202 end (54 mm) seamed on.
import math

# (radius, height) in metres, from the dome's middle under the can round to the lid's middle on top
PROFILE = [(0.0, 0.0105), (0.008, 0.0099), (0.015, 0.0082), (0.0205, 0.0052), (0.0236, 0.0016), (0.0242, 0.0),    # the dome, the foot
           (0.0252, 0.0002), (0.0275, 0.0026), (0.0305, 0.0066), (0.0322, 0.0105), (0.0330, 0.0150),                   # the base's taper
           (0.0330, 0.1030), (0.0326, 0.1060), (0.0310, 0.1100), (0.0288, 0.1140), (0.0268, 0.1168), (0.0262, 0.1180),  # the body, the shoulder, the neck
           (0.0268, 0.1190), (0.0272, 0.1205), (0.0272, 0.1216), (0.0266, 0.1222), (0.0258, 0.1216),                   # the seam's bead
           (0.0252, 0.1192), (0.0246, 0.1186), (0.0, 0.1190)]                                                          # the countersink, the lid
PRINT = (0.0150, 0.1060)   # the heights the print covers: the body, from the taper's top to the shoulder


def along():
    """Each point's distance along the outline, over its length (the lathe's v)."""
    lengths = [0.0]
    for (r0, y0), (r1, y1) in zip(PROFILE, PROFILE[1:]):
        lengths.append(lengths[-1] + math.hypot(r1 - r0, y1 - y0))
    return [l / lengths[-1] for l in lengths]


def print_band():
    """Where the print begins and ends along the outline (v, 0 under the can to 1 on its lid)."""
    v = along()
    heights = [y for _, y in PROFILE]
    return v[heights.index(PRINT[0])], v[heights.index(PRINT[1])]
