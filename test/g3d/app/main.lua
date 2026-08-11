-- g3d running as a wasmcart cart, with g3d/ copied VERBATIM from upstream.
--
-- The point of this cart is not that a cube appears. It is that an
-- unmodified third-party LOVE 3D library runs on this engine, which is the
-- whole claim the 3D support makes. Every love.* call g3d makes -- newMesh
-- with a custom vertex format, setDepthMode, a vertex shader written in
-- GLSL ES 1.00 spellings, a flat-16 mat4 send, package.loaded, getCanvas --
-- has to work as LOVE defines it, or g3d breaks and no amount of
-- engine-side special-casing would save it.
--
-- The scene is built to make depth VISIBLE rather than merely plausible:
-- two cubes overlap on screen, and the far one must be occluded by the
-- near one. Set NODEPTH=1 when packing (see run.sh) to turn the depth test
-- back off; the far cube then bleeds through, which is the control frame
-- the harness must be able to tell apart from this one.

local g3d = require "g3d"

local nearCube, farCube

-- A cube as a triangle list in g3d's vertex format:
--   {x, y, z, u, v, nx, ny, nz}
-- Built here rather than loaded from an .obj so the cart carries no binary
-- geometry and the mesh is inspectable in the diff.
local function cubeVerts(size)
    local s = size / 2
    local faces = {
        -- {origin, edge1, edge2, normal}
        {{-s,-s, s}, { 1, 0, 0}, { 0, 1, 0}, { 0, 0, 1}},  -- front
        {{ s,-s,-s}, {-1, 0, 0}, { 0, 1, 0}, { 0, 0,-1}},  -- back
        {{-s,-s,-s}, { 0, 0, 1}, { 0, 1, 0}, {-1, 0, 0}},  -- left
        {{ s,-s, s}, { 0, 0,-1}, { 0, 1, 0}, { 1, 0, 0}},  -- right
        {{-s, s, s}, { 1, 0, 0}, { 0, 0,-1}, { 0, 1, 0}},  -- top
        {{-s,-s,-s}, { 1, 0, 0}, { 0, 0, 1}, { 0,-1, 0}},  -- bottom
    }
    local verts = {}
    for _, f in ipairs(faces) do
        local o, e1, e2, n = f[1], f[2], f[3], f[4]
        -- two triangles per face, wound counter-clockwise seen from outside
        local corners = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} }
        for _, c in ipairs(corners) do
            local a, b = c[1], c[2]
            verts[#verts+1] = {
                o[1] + e1[1]*a*size + e2[1]*b*size,
                o[2] + e1[2]*a*size + e2[2]*b*size,
                o[3] + e1[3]*a*size + e2[3]*b*size,
                a, b,
                n[1], n[2], n[3],
            }
        end
    end
    return verts
end

function love.load()
    -- A checkerboard, so the texture path is exercised end to end: uv
    -- sampling, the standalone-texture upload that bypasses the 2D atlas,
    -- and mip generation. A flat colour would pass even if uv were broken.
    local tex = love.graphics.newImage("checker.png")

    -- NEAR cube: closer to the camera, drawn FIRST.
    nearCube = g3d.newModel(cubeVerts(1.0), tex, {0, 0, 0})
    -- FAR cube: behind the near one and overlapping it on screen, drawn
    -- SECOND. Painter's order would let it paint over the near cube; the
    -- depth test must not. The overlap IS the test, so the offset puts it
    -- behind the near cube from this camera rather than beside it.
    farCube  = g3d.newModel(cubeVerts(1.4), tex, {-0.75, 1.0, -0.6})

    -- NOT (0,0,4): g3d's default up vector is {0,0,1}, so looking straight
    -- down the Z axis makes up parallel to the view direction and
    -- cross(up, z) is the zero vector -- the view matrix comes out with two
    -- all-zero rows and every vertex collapses to x=0,y=0. No error, no GL
    -- warning, just an empty screen. Any off-axis eye position avoids it.
    g3d.camera.lookAt(2.5, -3.5, 2.0, 0, 0, 0)

    -- THE CONTROL, packed as an asset rather than a flag so the control and
    -- the real cart are the same Lua running under one difference.
    if wc.asset_exists and wc.asset_exists("NODEPTH") then
        love.graphics.setDepthMode()
        print("CONTROL: depth test disabled")
    end

    -- A fixed pose, not an animated one: the control frame and the real
    -- frame must differ ONLY in whether the depth test is on. If the cubes
    -- were spinning, any difference between the two screenshots could be
    -- explained away as a different moment in the animation.
    nearCube:setRotation(0, 0.54, 0)
    farCube:setRotation(0.36, 0.45, 0)
end

function love.draw()
    -- Draw order is deliberately near-then-far: see the header.
    nearCube:draw()
    farCube:draw()

    love.graphics.setColor(1, 1, 1)
    love.graphics.print("g3d on wasmcart-lua", 20, 20)
end
