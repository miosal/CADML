-- Involute spur-gear tooth profile generator.
--
-- Imported by gear.cadml via the frontmatter
--   import "gear.lua"
-- so the entry file can reach this module as `gear.involute_gear(...)`
-- and feed its returned path string directly into an <extrude>.

function involute_gear(mod, z, pa_deg, pts_per_flank)
    local pa = math.rad(pa_deg)
    local r_pitch = mod * z / 2
    local r_base  = r_pitch * math.cos(pa)
    local r_outer = r_pitch + mod
    local r_root  = r_pitch - 1.25 * mod

    -- Angular pitch (radians per tooth).
    local angular_pitch = 2 * math.pi / z

    -- Involute angle function: inv(a) = tan(a) - a.
    local function inv(a) return math.tan(a) - a end

    -- Half tooth thickness at the pitch circle.
    local tooth_angle = angular_pitch / 2

    -- Cartesian point on the involute at parameter t.
    local function involute_pt(t)
        local x = r_base * (math.cos(t) + t * math.sin(t))
        local y = r_base * (math.sin(t) - t * math.cos(t))
        return x, y
    end

    -- Largest involute parameter — reached at the outer radius.
    local t_max = math.sqrt(math.max((r_outer / r_base) ^ 2 - 1, 0))

    local profile = {}

    -- Each tooth: right flank involute -> tip arc -> left flank
    -- (mirror involute) -> root arc back to next tooth.
    for tooth = 0, z - 1 do
        local base_angle = tooth * angular_pitch
        local inv_pa     = inv(pa)
        local offset     = tooth_angle / 2 + inv_pa

        -- Right flank.
        for i = 0, pts_per_flank do
            local t = t_max * i / pts_per_flank
            local ix, iy = involute_pt(t)
            local ca = math.cos(base_angle + offset)
            local sa = math.sin(base_angle + offset)
            table.insert(profile, { ix * ca - iy * sa,
                                     ix * sa + iy * ca })
        end

        -- Tip arc (short circular segment at outer radius).
        --
        -- The polar angle of the involute curve at parameter t is
        -- (t - atan(t)) — that's where the right-flank's outermost
        -- vertex lands relative to the rotation origin. The tip arc
        -- has to start there (matching the previous segment's last
        -- point) and end at the symmetric position on the left
        -- flank's first point. Using atan(t_max) instead made the
        -- arc start ~30 deg off and produced a self-intersecting
        -- polygon that Manifold then rejected as non-manifold.
        local theta_outer = t_max - math.atan(t_max)
        local tip_start = base_angle + offset + theta_outer
        local tip_end   = base_angle + angular_pitch - offset - theta_outer
        for i = 0, 2 do
            local a = tip_start + (tip_end - tip_start) * i / 2
            table.insert(profile, { r_outer * math.cos(a),
                                     r_outer * math.sin(a) })
        end

        -- Left flank (mirror involute).
        for i = pts_per_flank, 0, -1 do
            local t = t_max * i / pts_per_flank
            local ix, iy = involute_pt(t)
            iy = -iy
            local mirror_offset = angular_pitch - offset
            local ca = math.cos(base_angle + mirror_offset)
            local sa = math.sin(base_angle + mirror_offset)
            table.insert(profile, { ix * ca - iy * sa,
                                     ix * sa + iy * ca })
        end

        -- Root arc to the next tooth.
        local root_start = base_angle + angular_pitch - offset
        local root_end   = base_angle + angular_pitch + offset
        for i = 0, 3 do
            local a = root_start + (root_end - root_start) * i / 3
            table.insert(profile, { r_root * math.cos(a),
                                     r_root * math.sin(a) })
        end
    end

    return cadml.path(profile)
end
