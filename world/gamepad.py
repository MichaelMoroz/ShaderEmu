# The two game controllers (docs/gamepad.md), each about its own middle: x right, y up (the
# sticks), z away from the player (the shoulder buttons). ShaderEmuModels.cs hangs them on
# the controllers the builder makes.


def pad(b, name, body, mark):
    b.obj(name)
    outline = [(-0.074, 0.030), (-0.046, 0.040), (0.046, 0.040), (0.074, 0.030), (0.080, 0.002), (0.071, -0.024), (0.044, -0.030),
               (0.026, -0.019), (-0.026, -0.019), (-0.044, -0.030), (-0.071, -0.024), (-0.080, 0.002)]
    b.prism((0, 0, 0), outline, 0.030, body, plane='xz', bevel=0.009, segs=4)
    for side in (-1, 1):
        # a grip, reaching back and down into the hand
        b.lathe((side * 0.060, -0.013, -0.046), [(0, -0.047), (0.013, -0.042), (0.020, -0.026), (0.0225, 0.0), (0.021, 0.028), (0.015, 0.041), (0, 0.046)],
                body, segs=20, rot=(-108, side * 12, 0))
        # two shoulder buttons, one over the other
        b.box((side * 0.052, 0.007, 0.040), (0.036, 0.011, 0.014), "PlasticDark", bevel=0.004, segs=3)
        b.box((side * 0.052, -0.007, 0.041), (0.032, 0.011, 0.014), "PlasticDark", bevel=0.004, segs=3)
        # a stick: a ring in the body, a stem, a dished cap
        b.lathe((side * 0.024, 0.0148, -0.010), [(0.0135, 0), (0.0135, 0.0018), (0.0115, 0.003), (0.0055, 0.0036), (0.0048, 0.011), (0.0095, 0.0125),
                                                 (0.0105, 0.016), (0.0085, 0.018), (0.004, 0.0172), (0, 0.0168)], "PlasticDark", segs=20)
        b.box((side * 0.009, 0.0152, 0.020), (0.011, 0.003, 0.0045), "PlasticDark", bevel=0.0014, segs=2)   # start, select
    # the cross, on a dish
    b.lathe((-0.047, 0.0148, 0.014), [(0.0165, 0), (0.0165, 0.0012), (0.015, 0.0018), (0, 0.0012)], "PlasticDark", segs=24)
    b.box((-0.047, 0.0172, 0.014), (0.027, 0.005, 0.0085), "PlasticDark", bevel=0.0018, segs=2)
    b.box((-0.047, 0.0172, 0.014), (0.0085, 0.005, 0.027), "PlasticDark", bevel=0.0018, segs=2)
    # four buttons, one in the controller's colour
    for i, (dx, dz) in enumerate(((-0.013, 0), (0.013, 0), (0, -0.013), (0, 0.013))):
        b.lathe((0.047 + dx, 0.0148, 0.014 + dz), [(0.0062, 0), (0.0062, 0.0022), (0.005, 0.0036), (0.0025, 0.0043), (0, 0.0045)],
                mark if i == 0 else "PlasticDark", segs=16)
    b.box((0, 0.0151, -0.002), (0.022, 0.0006, 0.007), mark, bevel=0.0002, segs=1)   # a badge between the sticks


def build(b):
    pad(b, "Gamepad shooter", "GamepadBody", "GamepadRed")
    pad(b, "Gamepad strategy", "GamepadBlue", "GamepadYellow")
