-- 5-inch drone propeller: Clark-Y airfoil with constant-pitch
-- helical twist. Each radial station gets a chord-scaled airfoil
-- in canonical (unrotated) orientation; the sketch's `rotation`
-- attribute applies the twist for that radius. Keeping rotation
-- at the sketch level (rather than inside the airfoil generator)
-- means consecutive stations' polygons stay vertex-paired
-- naturally — the loft's side faces twist smoothly along the
-- blade span instead of crossing.

-- ─── Parameters from CADML frontmatter ────────────────────────────

local R       = cadml.param("prop-r")     -- tip radius (mm)
local PITCH   = cadml.param("pitch")      -- propeller pitch (mm/turn)
local HUB_R   = cadml.param("hub-r")      -- hub radius (mm)
local HUB_H   = cadml.param("hub-h")      -- hub thickness (mm)

-- ─── Clark-Y airfoil approximation ────────────────────────────────
--
-- Camber: peak ~3.4 %c at ~40 %c, parabolic-ish.
-- Thickness: peak ~11.7 %c at ~30 %c, NACA-style 5-term polynomial.
-- Both vanish at LE (x=0) and TE (x=1).
local function clark_y_thickness(x)
    if x <= 0 or x >= 1 then return 0 end
    return 0.594689 * (
        0.298222773  * math.sqrt(x)
      - 0.127125232  * x
      - 0.357907906  * x ^ 2
      + 0.291984971  * x ^ 3
      - 0.105174606  * x ^ 4) * 0.117 / 0.06
end

local function clark_y_camber(x)
    if x <= 0 or x >= 1 then return 0 end
    return 0.034 * (1 - ((x - 0.40) / 0.40) ^ 2)
end

-- Generate a closed polygon path tracing the Clark-Y airfoil at
-- chord `c`. Cosine-spaced sampling concentrates points at the
-- LE and TE where curvature is highest; 40 samples per surface
-- gives smooth shape without exploding triangle counts.
--
-- Origin convention: x = 0 sits at the MID-CHORD point (50 %c
-- back from the LE), so x ∈ [-c/2, c/2]. The aerodynamically-
-- standard pitch axis is the quarter-chord (25 %c), but the chord
-- midpoint sits 0.25c aft of that — so a 25 %c-centred airfoil
-- rotated by the helical twist still skews the TE asymmetrically
-- relative to the LE. Mid-chord centring keeps the rotated chord
-- z-symmetric around the hub mid-plane: at 71° root pitch and
-- chord 4 mm, LE and TE land equidistant from each hub face.
function airfoil_section(c)
    local n = 40
    local x0 = c * 0.5  -- shift so mid-chord lands at x = 0
    local pts = {}
    -- Upper surface: LE -> TE.
    for i = 0, n do
        local f = 0.5 - 0.5 * math.cos(math.pi * i / n)
        local x = f * c - x0
        local y = (clark_y_camber(f) + clark_y_thickness(f) * 0.5) * c
        table.insert(pts, { x, y })
    end
    -- Lower surface: TE -> NEAR-LE (skip i=0 since upper already
    -- emitted the LE point).
    for i = n - 1, 1, -1 do
        local f = 0.5 - 0.5 * math.cos(math.pi * i / n)
        local x = f * c - x0
        local y = (clark_y_camber(f) - clark_y_thickness(f) * 0.5) * c
        table.insert(pts, { x, y })
    end
    return cadml.path(pts)
end

-- ─── Per-radial-station accessors ─────────────────────────────────

local R_MIN = HUB_R         -- blade root sits at hub radius
local R_TIP = R             -- blade tip at prop radius
local DR    = R_TIP - R_MIN

-- Chord: peaks roughly mid-span and tapers toward both ends.
function station_chord(t)
    local f = math.sin(math.pi * t ^ 0.85)
    return 4.0 + 7.5 * f
end

-- Helical-twist angle: beta(r) = atan(pitch / (2*pi*r)).
function station_beta(t)
    local r = R_MIN + DR * t
    if r < 1e-6 then r = 1e-6 end
    return math.deg(math.atan(PITCH / (2 * math.pi * r)))
end

-- Radial position of station t.
function station_r(t)
    return R_MIN + DR * t
end

-- Path d-string for station t (canonical, unrotated). The sketch's
-- `rotation=` attribute applies the twist.
function station_path(t)
    return airfoil_section(station_chord(t))
end
