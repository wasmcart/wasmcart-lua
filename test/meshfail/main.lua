-- meshfail/main.lua - newMesh must REFUSE what this engine cannot render.
--
-- The house rule is that an unsupported feature fails loudly with what to
-- use instead, never silently approximates. For meshes the tempting
-- approximations are all bad in the same way -- they render SOMETHING, so a
-- screenshot looks plausible while the cart's actual geometry was discarded:
--
--   * a custom vertex format: the renderer has ONE fixed vertex layout
--     (x,y,u,v,r,g,b,a) shared by every program including cart shaders, via
--     one shared VAO. Accepting a custom format would mean dropping the
--     cart's extra attributes on the floor and drawing the rest.
--   * the "points" draw mode: there is no point primitive on this path.
--     Quietly drawing it as a fan would fill a shape where the cart asked
--     for scattered dots.
--   * an unknown mode or usage string: a typo ("triangle" for "triangles",
--     "fans" for "fan") would otherwise fall through to the default and
--     render the wrong topology.
--
-- Each case logs REFUSED on the correct outcome and ACCEPTED on the wrong
-- one, so the gate reads the verdict rather than inferring it from an
-- absence. A cart that logged nothing at all would look identical to a cart
-- where every refusal worked, which is exactly the ambiguity to avoid.

local function must_refuse(what, fn)
  local ok, e = pcall(fn)
  if ok then
    print("ACCEPTED (should not have been): " .. what)
  else
    print("REFUSED " .. what .. ": " .. tostring(e))
  end
end

local function must_accept(what, fn)
  local ok, e = pcall(fn)
  if ok then
    print("ok " .. what)
  else
    print("ACCEPTED-FAILURE " .. what .. " was rejected but should work: " .. tostring(e))
  end
end

function love.load()
  -- 1: too MANY attributes. Custom attribute names are supported now (a
  --    declared format drives the buffer layout and the shader bindings),
  --    so the refusal that remains is the hard limit: 8 attribute slots,
  --    shared across the cart. Past that a format cannot be bound at all,
  --    and accepting it would mean silently dropping attributes.
  must_refuse("vertex format with more than 8 attributes", function()
    return love.graphics.newMesh({
      { "VertexPosition", "float", 3 }, { "VertexTexCoord", "float", 2 },
      { "VertexNormal", "float", 3 },   { "VertexColor", "byte", 4 },
      { "AttrE", "float", 1 }, { "AttrF", "float", 1 },
      { "AttrG", "float", 1 }, { "AttrH", "float", 1 },
      { "AttrI", "float", 1 },
    }, { { 0,0,0, 0,0, 0,0,1, 255,255,255,255, 0,0,0,0,0 } }, "triangles")
  end)

  -- 2: the "points" draw mode
  must_refuse("points draw mode", function()
    return love.graphics.newMesh({ { 0, 0 }, { 10, 0 }, { 10, 10 } }, "points")
  end)

  -- 3: a misspelled draw mode
  must_refuse("unknown draw mode", function()
    return love.graphics.newMesh({ { 0, 0 }, { 10, 0 }, { 10, 10 } }, "triangle")
  end)

  -- 4: a misspelled usage hint
  must_refuse("unknown usage", function()
    return love.graphics.newMesh({ { 0, 0 }, { 10, 0 }, { 10, 10 } }, "fan", "streaming")
  end)

  -- 5: an empty vertex list. A zero-vertex mesh has no geometry, and
  --    returning one would defer the error to a draw that renders nothing.
  must_refuse("empty vertex list", function()
    return love.graphics.newMesh({})
  end)

  -- 6: a non-Image passed to setTexture
  must_refuse("setTexture(non-image)", function()
    local m = love.graphics.newMesh({ { 0, 0 }, { 10, 0 }, { 10, 10 } })
    m:setTexture("tex.png")
  end)

  -- 7: a normal without a 3D position. VertexNormal only exists in the 3D
  --    layout, so pairing it with a 2-component position describes a vertex
  --    this engine has nowhere to put.
  must_refuse("VertexNormal with a 2D position", function()
    return love.graphics.newMesh({
      { "VertexPosition", "float", 2 },
      { "VertexNormal",   "float", 3 },
    }, { { 0, 0, 0, 0, 0 } }, "triangles")
  end)

  -- 8: a 3D mesh from a vertex COUNT. Its buffer is uploaded once at
  --    creation and there is no per-vertex write path to fill in later, so
  --    a count-only 3D mesh could only ever draw nothing.
  must_refuse("3D mesh from a vertex count", function()
    return love.graphics.newMesh({
      { "VertexPosition", "float", 3 },
      { "VertexTexCoord", "float", 2 },
      { "VertexNormal",   "float", 3 },
      { "VertexColor",    "byte",  4 },
    }, 8, "triangles")
  end)

  -- 9: a 3D mesh in a non-triangles mode. Fans and strips are 2D
  --    conveniences; reinterpreting the cart's topology would silently
  --    render different geometry than it built.
  must_refuse("3D mesh with a fan draw mode", function()
    return love.graphics.newMesh({
      { "VertexPosition", "float", 3 },
      { "VertexNormal",   "float", 3 },
    }, { { 0,0,0, 0,0,1 }, { 1,0,0, 0,0,1 }, { 0,1,0, 0,0,1 } }, "fan")
  end)

  -- and the control: a GOOD mesh must still be accepted. Without this the
  -- gate would pass just as well if newMesh refused everything.
  must_accept("a valid default-format mesh", function()
    local m = love.graphics.newMesh({
      {  0,  0, 0, 0, 1, 1, 1, 1 },
      { 40,  0, 1, 0, 1, 1, 1, 1 },
      { 40, 40, 1, 1, 1, 1, 1, 1 },
    }, "fan", "dynamic")
    if m:getVertexCount() ~= 3 then error("vertex count wrong") end
    if m:getDrawMode() ~= "fan" then error("draw mode not reported") end
    return m
  end)

  -- the SAME control for 3D. Without it, every refusal above would still
  -- pass if declared formats were rejected outright, which is exactly the
  -- state this engine used to be in.
  must_accept("a valid 3D-format mesh", function()
    local m = love.graphics.newMesh({
      { "VertexPosition", "float", 3 },
      { "VertexTexCoord", "float", 2 },
      { "VertexNormal",   "float", 3 },
      { "VertexColor",    "byte",  4 },
    }, {
      { -1, -1, 0,  0, 0,  0, 0, 1,  1, 1, 1, 1 },
      {  1, -1, 0,  1, 0,  0, 0, 1,  1, 1, 1, 1 },
      {  0,  1, 0,  0.5, 1, 0, 0, 1, 1, 1, 1, 1 },
    }, "triangles")
    if m:getVertexCount() ~= 3 then error("vertex count wrong") end
    if m:type() ~= "Mesh" then error("a 3D mesh must still report type Mesh") end
    return m
  end)

  -- A CUSTOM attribute must be accepted. This is what a real renderer needs
  -- (a tangent for normal mapping, material terms for PBR), and refusing it
  -- was the wall that stopped 3DreamEngine from building a single mesh.
  must_accept("a vertex format with custom attributes", function()
    local m = love.graphics.newMesh({
      { "VertexPosition", "float", 4 },
      { "VertexTexCoord", "float", 2 },
      { "VertexNormal",   "byte",  4 },
      { "VertexTangent",  "byte",  4 },
    }, {
      { 0,0,0,1,  0,0,  128,128,255,0,  255,128,128,0 },
      { 1,0,0,1,  1,0,  128,128,255,0,  255,128,128,0 },
      { 0,1,0,1,  0,1,  128,128,255,0,  255,128,128,0 },
    }, "triangles")
    if m:getVertexCount() ~= 3 then error("vertex count wrong") end
    return m
  end)

  -- A 2D-shaped declared format must route to the 2D path, not be refused:
  -- it describes exactly the engine's built-in layout, spelled out.
  must_accept("a declared 2D-format mesh", function()
    return love.graphics.newMesh({
      { "VertexPosition", "float", 2 },
      { "VertexTexCoord", "float", 2 },
      { "VertexColor",    "byte",  4 },
    }, {
      {  0,  0, 0, 0, 1, 1, 1, 1 },
      { 40,  0, 1, 0, 1, 1, 1, 1 },
      { 40, 40, 1, 1, 1, 1, 1, 1 },
    }, "fan")
  end)

  -- ── semantics a pixel probe cannot see ───────────────────────────
  --
  -- Every index in LOVE's mesh API is 1-BASED and every index in the C
  -- storage is 0-based. That conversion happens in exactly one place per
  -- call, and an off-by-one there is nearly invisible on screen: a mesh
  -- drawn from vertex 2 onwards still looks like a mesh. Round-tripping the
  -- getters is the only way to see it.
  local function check(what, got, want)
    if math.abs(got - want) > 0.001 then
      print("BADVALUE " .. what .. ": got " .. tostring(got) .. " want " .. tostring(want))
    end
  end

  local m = love.graphics.newMesh(4, "triangles")
  -- a mesh made from a COUNT must default to opaque white, or a cart that
  -- sets only positions would draw nothing at all
  local _, _, _, _, r, g, b, a = m:getVertex(1)
  check("default r", r, 1); check("default g", g, 1)
  check("default b", b, 1); check("default a", a, 1)

  m:setVertex(1, 11, 22, 0.25, 0.5, 0.1, 0.2, 0.3, 0.4)
  local vx, vy, vu, vv, vr, vg, vb, va = m:getVertex(1)
  check("setVertex x", vx, 11);   check("setVertex y", vy, 22)
  check("setVertex u", vu, 0.25); check("setVertex v", vv, 0.5)
  check("setVertex r", vr, 0.1);  check("setVertex a", va, 0.4)

  -- vertex 1 must be a DIFFERENT vertex from vertex 2: if the 1-based to
  -- 0-based conversion were missing, writing 1 would land on index 1 and
  -- vertex 2 would come back with the values just written.
  local ox = m:getVertex(2)
  check("setVertex(1) did not touch vertex 2", ox, 0)

  -- the table form of setVertex must agree with the positional form
  m:setVertex(2, { 33, 44, 0, 0, 1, 1, 1, 1 })
  local tx, ty = m:getVertex(2)
  check("setVertex table x", tx, 33); check("setVertex table y", ty, 44)

  -- setVertices with a start index, also 1-based
  m:setVertices({ { 55, 66, 0, 0, 1, 1, 1, 1 } }, 3)
  local sx3 = m:getVertex(3)
  check("setVertices start index", sx3, 55)

  -- the vertex map round-trips in LOVE's 1-based space
  m:setVertexMap({ 1, 2, 3, 3, 2, 1 })
  local map = m:getVertexMap()
  if not map then
    print("BADVALUE getVertexMap returned nil after setVertexMap")
  else
    check("map length", #map, 6)
    check("map[1]", map[1], 1)
    check("map[4]", map[4], 3)
    check("map[6]", map[6], 1)
  end
  m:setVertexMap(nil)
  if m:getVertexMap() ~= nil then print("BADVALUE getVertexMap not cleared") end

  -- the draw range likewise
  m:setDrawRange(2, 2)
  local rs, rc = m:getDrawRange()
  check("drawRange start", rs, 2); check("drawRange count", rc, 2)
  m:setDrawRange()
  if m:getDrawRange() ~= nil then print("BADVALUE getDrawRange not cleared") end

  check("getVertexCount", m:getVertexCount(), 4)
  if m:type() ~= "Mesh" then print("BADVALUE type() is not Mesh") end
  if m:getTexture() ~= nil then print("BADVALUE getTexture should start nil") end
  print("ok mesh index semantics")
end

function love.draw()
  love.graphics.setBackgroundColor(0.1, 0.1, 0.15)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("meshfail", 20, 20)
end
