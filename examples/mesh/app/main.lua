-- mesh/main.lua - love.graphics.newMesh on the GL2D backend.
--
-- Like the shaders example, this cart is built so that a WRONG result is
-- visible rather than merely absent. A mesh that fails to draw leaves the
-- frame count perfect and a hole in the screen, so every panel here has
-- something next to it that must MATCH or CONTRAST in a specific way.
--
-- Four panels, each proving one thing:
--
--   1  TEXTURED MESH vs the same image drawn as a sprite.
--      Sprites live in a shared 2048^2 atlas, so a mesh's 0..1 uv has to be
--      remapped into that sub-rect. Get the remap wrong and the mesh shows a
--      garbled crop, or the NEIGHBOURING image (decoy.png is loaded first and
--      sits next to tex.png in the atlas, so a stale offset lands on stripes
--      that cannot be mistaken for the quadrant texture). The reference
--      sprite is drawn immediately below at the same size: the two must be
--      pixel-for-pixel the same picture.
--
--   2  PER-VERTEX COLOUR. A quad whose four corners are red/green/blue/white
--      with no texture at all. If the colour attribute is not reaching the
--      shader this is a flat block; if it is, it is a smooth bilinear blend.
--      Flat-vs-blended is the assertion.
--
--   3  "triangles" vs "fan" ON THE SAME SIX VERTICES. Both panels get the
--      identical vertex list. In "triangles" mode that is two separate
--      triangles with a gap between them; in "fan" mode it is one solid
--      four-triangle fan around vertex 1. If the mode were ignored the two
--      panels would look the same, which is exactly what this catches -- a
--      mode that silently defaults to fan is otherwise invisible.
--
--   4  A UV SUB-RECT plus a vertex map. The mesh samples only the top-left
--      quadrant of the texture (uv 0..0.5), which under a correct atlas
--      remap is the red quadrant with the black L bracket. Sampling the
--      whole atlas instead would put most of a 2048px texture in there and
--      show almost nothing recognisable.
--
-- The strip along the bottom is a textured "strip" mesh with a per-vertex
-- alpha ramp, so blending and strip winding are on screen too.

local tex, decoy
local texMesh, colorMesh, triMesh, fanMesh, uvMesh, stripMesh
local t = 0

-- The six vertices panel 3 shares between its two meshes. Two disjoint
-- triangles when read as "triangles"; one fan when read as "fan".
local SHARED = {
  --  x    y    u  v   r  g  b  a
  {  60,   0,   0, 0,  1.0, 0.35, 0.30, 1 },
  {   0,  90,   0, 0,  1.0, 0.35, 0.30, 1 },
  { 120,  90,   0, 0,  1.0, 0.35, 0.30, 1 },

  { 120, 110,   0, 0,  0.35, 0.75, 1.0, 1 },
  {   0, 110,   0, 0,  0.35, 0.75, 1.0, 1 },
  {  60, 200,   0, 0,  0.35, 0.75, 1.0, 1 },
}

function love.load()
  love.graphics.setBackgroundColor(0.07, 0.08, 0.12)

  -- decoy FIRST, so it occupies the atlas slot before tex.png. A mesh that
  -- ignores the atlas offset samples from (0,0) and lands on these stripes.
  decoy = love.graphics.newImage("decoy.png")
  tex   = love.graphics.newImage("tex.png")

  -- 1: a textured unit quad, "fan", 0..1 uv over the whole image
  texMesh = love.graphics.newMesh({
    {   0,   0,  0, 0,  1, 1, 1, 1 },
    { 180,   0,  1, 0,  1, 1, 1, 1 },
    { 180, 180,  1, 1,  1, 1, 1, 1 },
    {   0, 180,  0, 1,  1, 1, 1, 1 },
  }, "fan")
  texMesh:setTexture(tex)

  -- 2: per-vertex colour, no texture
  colorMesh = love.graphics.newMesh({
    {   0,   0,  0, 0,  1, 0.15, 0.15, 1 },
    { 180,   0,  0, 0,  0.15, 1, 0.30, 1 },
    { 180, 180,  0, 0,  0.25, 0.40, 1, 1 },
    {   0, 180,  0, 0,  1, 1, 1, 1 },
  }, "fan")

  -- 3: THE SAME SIX VERTICES, two different modes
  triMesh = love.graphics.newMesh(SHARED, "triangles")
  fanMesh = love.graphics.newMesh(SHARED, "fan")

  -- 4: a uv sub-rect (the texture's top-left quadrant only) drawn through a
  --    vertex map, so the index buffer is exercised on a textured draw
  uvMesh = love.graphics.newMesh({
    {   0,   0,  0.0, 0.0,  1, 1, 1, 1 },
    { 180,   0,  0.5, 0.0,  1, 1, 1, 1 },
    { 180, 180,  0.5, 0.5,  1, 1, 1, 1 },
    {   0, 180,  0.0, 0.5,  1, 1, 1, 1 },
  }, "triangles")
  uvMesh:setTexture(tex)
  -- two triangles from four vertices: only possible via the map
  uvMesh:setVertexMap({ 1, 2, 3, 1, 3, 4 })

  -- the bottom strip: textured, with a per-vertex alpha ramp
  local sv = {}
  local N = 24
  for i = 0, N do
    local x = i * (1180 / N)
    local u = i / N
    local a = 0.25 + 0.75 * (i / N)
    sv[#sv + 1] = { x,  0, u, 0, 1, 1, 1, a }
    sv[#sv + 1] = { x, 70, u, 1, 1, 1, 1, a }
  end
  stripMesh = love.graphics.newMesh(sv, "strip")
  stripMesh:setTexture(tex)
end

local function label(s, x, y)
  love.graphics.setColor(0.85, 0.88, 0.95)
  love.graphics.print(s, x, y)
end

function love.draw()
  t = t + 1 / 60

  love.graphics.setColor(1, 1, 1)

  -- ── panel 1: textured mesh, with the sprite reference beneath it ──
  label("1 textured mesh", 40, 30)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(texMesh, 40, 56)
  label("same image as a sprite (must match)", 40, 250)
  -- 180/64 = 2.8125: the same destination size the mesh covers
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(tex, 40, 276, 0, 180 / 64, 180 / 64)

  -- ── panel 2: per-vertex colour ────────────────────────────────────
  label("2 per-vertex colour", 300, 30)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(colorMesh, 300, 56)

  -- ── panel 3: the SAME vertices in two modes ───────────────────────
  label("3 \"triangles\"", 560, 30)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(triMesh, 570, 76)
  label("3 \"fan\" (same verts)", 730, 300)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(fanMesh, 750, 76)

  -- ── panel 4: uv sub-rect through a vertex map ─────────────────────
  label("4 uv 0..0.5 + vertex map", 960, 30)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(uvMesh, 960, 56)

  -- the decoy, drawn small, so the atlas genuinely holds two images and the
  -- screenshot shows what a wrong uv remap would have looked like
  label("decoy (atlas neighbour)", 960, 250)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(decoy, 960, 276, 0, 1.5, 1.5)

  -- ── transform + tint: the mesh must honour both ───────────────────
  label("5 rotated + scaled + tinted", 300, 300)
  love.graphics.push()
  love.graphics.translate(390, 400)
  love.graphics.rotate(t * 0.7)
  love.graphics.setColor(0.6, 1.0, 0.7)
  love.graphics.draw(texMesh, 0, 0, 0, 0.5, 0.5, 90, 90)
  love.graphics.pop()

  -- ── the strip, with an alpha ramp ─────────────────────────────────
  love.graphics.setColor(1, 1, 1)
  label("6 \"strip\" mesh, per-vertex alpha ramp left to right", 50, 560)
  love.graphics.setColor(1, 1, 1)
  love.graphics.draw(stripMesh, 50, 586)
end
