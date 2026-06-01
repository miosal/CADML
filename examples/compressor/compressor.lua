-- Centrifugal turbo compressor wheel - parametric shape generator.
--
-- Imported by compressor.cadml via `import "compressor.lua"`. The
-- CADML side uses these functions to author a swept hub revolve + N
-- backward-curved blades via `<loft>` over `<for>`-unrolled `<sketch>`
-- cross-sections.

-- Hub meridional profile parameters
local H_R0  = cadml.param("hub-inlet-r")
local H_R1  = cadml.param("exducer-r")
local H_H   = cadml.param("height")
local H_DR  = H_R1 - H_R0
local TIP_R = cadml.param("inducer-tip-r")

-- CONCAVE hub meridional curve.
-- z drops quickly (nearly vertical inducer), r expands late (radial
-- exducer). Different exponents create concave curvature in r-z
-- space. t: 0 = inducer (top), 1 = exducer (bottom).
function hub_r(t) return H_R0 + H_DR * t ^ 2.4 end
function hub_z(t) return H_H * (1 - t ^ 0.65) end

-- Blade height (hub to shroud) — tapers from inducer to exducer.
function blade_h(t) return (TIP_R - H_R0) * (1 - 0.55 * t) end

-- Blade angle: +25 deg at inducer (forward lean), -55 deg at
-- exducer (backsweep).
function blade_beta(t) return 25 - 80 * t ^ 1.3 end

-- Blade thickness: thin realistic profile, peaks mid-blade.
function blade_t(t) return 0.8 + 0.6 * math.sin(math.pi * t) end

-- Negated hub meridional tangent direction. The loft uses
-- right = up x normal, so we negate the tangent to make blade span
-- (right direction) point radially outward.
function hub_tx(t)
    local dt = 0.001
    local t0 = math.max(t - dt, 0)
    local t1 = math.min(t + dt, 1)
    local dr = hub_r(t1) - hub_r(t0)
    local dz = hub_z(t1) - hub_z(t0)
    local len = math.sqrt(dr * dr + dz * dz)
    if len < 1e-9 then return 0 end
    return -dr / len
end

function hub_tz(t)
    local dt = 0.001
    local t0 = math.max(t - dt, 0)
    local t1 = math.min(t + dt, 1)
    local dr = hub_r(t1) - hub_r(t0)
    local dz = hub_z(t1) - hub_z(t0)
    local len = math.sqrt(dr * dr + dz * dz)
    if len < 1e-9 then return 1 end
    return -dz / len
end

-- Hub meridional profile for revolve (x = radius, y = z-height).
function hub_profile()
    local bore_r = cadml.param("bore-r")
    local bt     = cadml.param("back-t")
    local pts = {}
    -- Top: bore to hub inlet with fillet.
    table.insert(pts, { bore_r,    H_H + 2 })
    table.insert(pts, { H_R0 - 0.5, H_H + 2 })
    table.insert(pts, { H_R0,      H_H + 1.5 })
    table.insert(pts, { H_R0,      H_H })
    -- Concave meridional curve.
    for i = 1, 50 do
        local t = i / 50
        table.insert(pts, { hub_r(t), hub_z(t) })
    end
    -- Backplate.
    table.insert(pts, { H_R1,   -bt })
    table.insert(pts, { bore_r, -bt })
    return cadml.path(pts)
end

-- Blade cross-section: thin plate with rounded LE and tapered root.
function blade_section(h, t)
    local n = 30
    local pts = {}
    local le_r = t * 0.4
    -- Lower surface (root to tip).
    for i = 0, n do
        local f = i / n
        local tap = math.min(f / 0.15, 1.0)
        local tip = 1.0 - 0.3 * math.max(f - 0.85, 0) / 0.15
        table.insert(pts, { f * h, -t / 2 * tap * tip })
    end
    -- Rounded leading edge (tip).
    for i = 1, 8 do
        local a = -math.pi / 2 + math.pi * i / 9
        table.insert(pts, { h + le_r * math.cos(a), le_r * math.sin(a) })
    end
    -- Upper surface (tip to root).
    for i = n, 0, -1 do
        local f = i / n
        local tap = math.min(f / 0.15, 1.0)
        local tip = 1.0 - 0.3 * math.max(f - 0.85, 0) / 0.15
        table.insert(pts, { f * h, t / 2 * tap * tip })
    end
    return cadml.path(pts)
end
