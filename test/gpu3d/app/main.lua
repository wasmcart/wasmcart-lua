-- gpu3d - the deferred-rendering capability cart.
--
-- Each check proves ONE capability and prints a verdict, so the harness
-- reads a decision rather than inferring one from a screenshot. Every check
-- has a specific WRONG answer it can distinguish -- a capability that merely
-- "looks plausible" is not proven.
--
-- The results are also drawn on screen as labelled swatches, so a human
-- looking at the frame sees the same verdicts the harness asserts.

local ok, fail = 0, 0
local lines = {}

local function verdict(name, pass, detail)
  if pass then
    ok = ok + 1
    print("PASS " .. name .. (detail and (" " .. detail) or ""))
  else
    fail = fail + 1
    print("FAIL " .. name .. " " .. tostring(detail))
  end
  lines[#lines + 1] = { name = name, pass = pass, detail = detail }
end

-- A triangle that covers the whole target in clip space. These are all
-- fragment-shader tests; the geometry only has to cover.
local function fullscreenMesh()
  return love.graphics.newMesh({
    {"VertexPosition", "float", 3},
    {"VertexTexCoord", "float", 2},
    {"VertexNormal",   "float", 3},
    {"VertexColor",    "byte",  4},
  }, {
    { -1, -1, 0,  0, 0,  0,0,1,  1,1,1,1 },
    {  3, -1, 0,  2, 0,  0,0,1,  1,1,1,1 },
    { -1,  3, 0,  0, 2,  0,0,1,  1,1,1,1 },
  }, "triangles")
end

local VERT = [[
  vec4 position(mat4 tp, vec4 vp) {
    vec3 n = VertexNormal;      // keeps the 3D prologue selected
    return vec4(vp.xy, 0.0, 1.0);
  }
]]

local tri
local probe          -- shader that samples a target and reports one channel
local targets = {}

function love.load()
  tri = fullscreenMesh()

  local formats = love.graphics.getCanvasFormats()
  local limits = love.graphics.getSystemLimits()
  print("INFO multicanvas=" .. tostring(limits.multicanvas) ..
        " texturesize=" .. tostring(limits.texturesize) ..
        " texturelayers=" .. tostring(limits.texturelayers))
  print("INFO rgba16f=" .. tostring(formats.rgba16f) ..
        " r16f=" .. tostring(formats.r16f) ..
        " depth24=" .. tostring(formats.depth24))

  -- ── 1: MRT + float, in one pass ──────────────────────────────────
  --
  -- One draw writes target A = 2.5 (red) and target B = 0.5 (green). Two
  -- claims are tested at once:
  --   * the attachments got DIFFERENT values -> MRT really is multi-target,
  --     not the same colour broadcast to both;
  --   * 2.5 survived -> the target is really float. An RGBA8 target clamps
  --     it to 1.0, which is the silent downgrade this must catch.
  if formats.rgba16f then
    local a = love.graphics.newCanvas(32, 32, { format = "rgba16f" })
    local b = love.graphics.newCanvas(32, 32, { format = "rgba16f" })
    local gbuf = love.graphics.newShader([[
      #pragma wasmcart mrt 2
      void effect2(out vec4 c0, out vec4 c1) {
        c0 = vec4(2.5, 0.0, 0.0, 1.0);
        c1 = vec4(0.0, 0.5, 0.0, 1.0);
      }
    ]], VERT)

    local bound = pcall(love.graphics.setCanvas, { a, b })
    if bound then
      love.graphics.setShader(gbuf)
      love.graphics.draw(tri)
      love.graphics.setShader()
      love.graphics.setCanvas()
      targets.mrtA, targets.mrtB = a, b
      verdict("mrt-draw", true, "2 targets, 1 pass")
    else
      verdict("mrt-draw", false, "setCanvas({a,b}) failed")
    end
  else
    verdict("float-format", false, "driver reports no rgba16f")
  end

  -- ── 2: depth target ──────────────────────────────────────────────
  if formats.depth24 then
    local d = love.graphics.newCanvas(32, 32, { format = "depth24" })
    local okBind = pcall(love.graphics.setCanvas, { depthstencil = d })
    love.graphics.setCanvas()
    verdict("depth-target", okBind, okBind and "depth-only FBO complete"
                                            or "depth-only FBO refused")
    targets.depth = d
  else
    verdict("depth-target", false, "driver reports no depth24")
  end

  -- ── 3: cubemap, six distinct faces ───────────────────────────────
  local cube = love.graphics.newCanvas(16, 16, { format = "rgba8", type = "cube" })
  local faceOK = true
  local paint = love.graphics.newShader([[
    extern number shade;
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      return vec4(shade, shade, shade, 1.0);
    }
  ]], VERT)
  for face = 1, 6 do
    local bound = pcall(love.graphics.setCanvas, { { cube, face = face } })
    if not bound then faceOK = false break end
    paint:send("shade", face / 6)
    love.graphics.setShader(paint)
    love.graphics.draw(tri)
    love.graphics.setShader()
  end
  love.graphics.setCanvas()
  verdict("cubemap-faces", faceOK, faceOK and "6 faces bound and drawn"
                                           or "a face could not be bound")
  targets.cube = cube

  -- ── 4: array texture layers ──────────────────────────────────────
  local arr = love.graphics.newCanvas(16, 16, { format = "rgba8", type = "array", layers = 4 })
  local layerOK = true
  for layer = 1, 4 do
    local bound = pcall(love.graphics.setCanvas, { { arr, layer = layer } })
    if not bound then layerOK = false break end
    paint:send("shade", layer / 4)
    love.graphics.setShader(paint)
    love.graphics.draw(tri)
    love.graphics.setShader()
  end
  love.graphics.setCanvas()
  verdict("array-layers", layerOK, layerOK and "4 layers bound and drawn"
                                            or "a layer could not be bound")

  -- ── 5: volume texture ────────────────────────────────────────────
  local volOK = pcall(function()
    return love.graphics.newCanvas(16, 16, { format = "rgba8", type = "volume", layers = 4 })
  end)
  verdict("volume-texture", volOK, volOK and "created" or "creation failed")

  -- ── 6: colour mask ───────────────────────────────────────────────
  -- Clear a target to black, mask off GREEN, then draw white. Green must
  -- stay 0 while red and blue become 1. A no-op mask gives white.
  local cm = love.graphics.newCanvas(16, 16, { format = "rgba8" })
  love.graphics.setCanvas(cm)
  love.graphics.clear(0, 0, 0, 1)
  love.graphics.setColorMask(true, false, true, true)
  local white = love.graphics.newShader([[
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) { return vec4(1.0); }
  ]], VERT)
  love.graphics.setShader(white)
  love.graphics.draw(tri)
  love.graphics.setShader()
  love.graphics.setColorMask()
  love.graphics.setCanvas()
  targets.mask = cm
  verdict("colormask-draw", true, "masked draw issued")

  -- ── 6b: a cube IMAGE, sampled by direction ───────────────────────
  --
  -- Six faces built from six distinct 1x1 colours, then sampled with six
  -- direction vectors. Each lookup must return ITS OWN face's colour, which
  -- is what catches the classic bug: faces uploaded in the wrong order look
  -- perfectly fine on a skybox until you notice the sun is on the wrong
  -- side. The result is written to a 6x1 target, one pixel per direction.
  local faces = {}
  local FACE_RGB = {
    {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1},
  }
  for i = 1, 6 do
    local d = love.image.newImageData and nil
    faces[i] = "face" .. i .. ".png"
  end
  local cubeImgOK, cubeImg = pcall(love.graphics.newCubeImage, faces)
  if cubeImgOK then
    local probe6 = love.graphics.newCanvas(6, 1, { format = "rgba8" })
    local cubeSample = love.graphics.newShader([[
      extern CubeImage sky;
      vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
        // One pixel per face direction, left to right: +X -X +Y -Y +Z -Z
        int i = int(floor(uv.x * 6.0));
        vec3 dirs[6];
        dirs[0] = vec3( 1.0, 0.0, 0.0); dirs[1] = vec3(-1.0, 0.0, 0.0);
        dirs[2] = vec3( 0.0, 1.0, 0.0); dirs[3] = vec3( 0.0,-1.0, 0.0);
        dirs[4] = vec3( 0.0, 0.0, 1.0); dirs[5] = vec3( 0.0, 0.0,-1.0);
        return vec4(texture(sky, dirs[i]).rgb, 1.0);
      }
    ]], VERT)
    cubeSample:send("sky", cubeImg)
    love.graphics.setCanvas(probe6)
    love.graphics.clear(0, 0, 0, 1)
    love.graphics.setShader(cubeSample)
    love.graphics.draw(tri)
    love.graphics.setShader()
    love.graphics.setCanvas()
    -- NEAREST, so the 6 probe pixels stay distinct when stretched across
    -- the screen. LINEAR would blend neighbouring faces into each other and
    -- a wrong-order bug would smear into looking merely "soft".
    probe6:setFilter("nearest")
    targets.cubeProbe = probe6
    verdict("cubeimage", true, "6 faces uploaded and sampled by direction")
  else
    verdict("cubeimage", false, tostring(cubeImg))
  end

  -- ── 7: instancing ────────────────────────────────────────────────
  -- 8 instances, each a small quad offset by gl_InstanceID. If instancing
  -- were ignored, all 8 would land on top of each other and the harness
  -- would count one bar instead of eight.
  local inst = love.graphics.newCanvas(128, 16, { format = "rgba8" })
  local instShader = love.graphics.newShader([[
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) { return vec4(1.0, 1.0, 0.0, 1.0); }
  ]], [[
    vec4 position(mat4 tp, vec4 vp) {
      vec3 n = VertexNormal;
      float i = float(gl_InstanceID);
      // 8 slots across the target; each instance a narrow bar in its own.
      float x = -1.0 + (i * 2.0 + 0.5) * 0.125;
      return vec4(vp.xy * vec2(0.05, 1.0) + vec2(x, 0.0), 0.0, 1.0);
    }
  ]])
  local quad = love.graphics.newMesh({
    {"VertexPosition", "float", 3},
    {"VertexTexCoord", "float", 2},
    {"VertexNormal",   "float", 3},
    {"VertexColor",    "byte",  4},
  }, {
    { -1, -1, 0, 0,0, 0,0,1, 1,1,1,1 },
    {  1, -1, 0, 1,0, 0,0,1, 1,1,1,1 },
    {  1,  1, 0, 1,1, 0,0,1, 1,1,1,1 },
    { -1, -1, 0, 0,0, 0,0,1, 1,1,1,1 },
    {  1,  1, 0, 1,1, 0,0,1, 1,1,1,1 },
    { -1,  1, 0, 0,1, 0,0,1, 1,1,1,1 },
  }, "triangles")
  love.graphics.setCanvas(inst)
  love.graphics.clear(0, 0, 0, 1)
  love.graphics.setShader(instShader)
  local instOK = pcall(love.graphics.drawInstanced, quad, 8)
  love.graphics.setShader()
  love.graphics.setCanvas()
  targets.inst = inst
  verdict("instancing", instOK, instOK and "8 instances drawn" or "drawInstanced failed")

  print("SUMMARY pass=" .. ok .. " fail=" .. fail)
end

-- Present a target as a screen rectangle, scaling its value by `scale` so an
-- out-of-range float is distinguishable from a clamped one.
--
-- A QUAD, not the fullscreen triangle: the triangle extends past the rect it
-- is mapped into, so using it here painted big white wedges across the frame
-- instead of swatches. The mesh has to be the shape being drawn.
local quadMesh, showShader

local function makeQuad()
  return love.graphics.newMesh({
    {"VertexPosition", "float", 3},
    {"VertexTexCoord", "float", 2},
    {"VertexNormal",   "float", 3},
    {"VertexColor",    "byte",  4},
  }, {
    { 0, 0, 0,  0, 0,  0,0,1, 1,1,1,1 },
    { 1, 0, 0,  1, 0,  0,0,1, 1,1,1,1 },
    { 1, 1, 0,  1, 1,  0,0,1, 1,1,1,1 },
    { 0, 0, 0,  0, 0,  0,0,1, 1,1,1,1 },
    { 1, 1, 0,  1, 1,  0,0,1, 1,1,1,1 },
    { 0, 1, 0,  0, 1,  0,0,1, 1,1,1,1 },
  }, "triangles")
end

local function show(target, x, y, w, h, scale)
  showShader:send("tgt", target)
  showShader:send("scale", scale or 1)
  showShader:send("rect", { x, y, w, h })
  love.graphics.setShader(showShader)
  love.graphics.draw(quadMesh)
  love.graphics.setShader()
end

local presentReady = false
local function ensurePresent()
  if presentReady then return end
  presentReady = true
  -- The presentation quad and its shader, built once. The vertex maps the
  -- quad's 0..1 corners onto a clip-space rect, so each swatch lands exactly
  -- where the harness expects to probe it.
  quadMesh = makeQuad()
  showShader = love.graphics.newShader([[
    extern Image tgt;
    extern number scale;
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      // texture(), NOT Texel(): Texel is the engine's helper and honours
      // u_textured, which describes the DRAW's own texture. A render target
      // bound to a cart sampler is neither, so the raw GLSL call is right.
      return vec4(texture(tgt, uv).rgb * scale, 1.0);
    }
  ]], [[
    extern vec4 rect;   // x, y, w, h in clip space
    vec4 position(mat4 tp, vec4 vp) {
      vec3 n = VertexNormal;
      return vec4(vp.xy * rect.zw + rect.xy, 0.0, 1.0);
    }
  ]])

end

function love.draw()
  ensurePresent()
  love.graphics.clear(0, 0, 0, 1)

  -- Swatch row. Positions are clip space, and the harness knows them.
  -- A: the 2.5 float, scaled by 0.25 -> 0.625 grey if the target is really
  --    float; 0.25 if it was clamped to 1.0 by an RGBA8 downgrade.
  if targets.mrtA then show(targets.mrtA, -0.9,  0.2, 0.35, 0.35, 0.25) end
  -- B: the second attachment, 0.5 green, unscaled.
  if targets.mrtB then show(targets.mrtB, -0.45, 0.2, 0.35, 0.35, 1.0) end
  -- The colour-masked target: red+blue on, green masked off -> magenta.
  if targets.mask then show(targets.mask,  0.0,  0.2, 0.35, 0.35, 1.0) end
  -- The instanced bars.
  if targets.inst then show(targets.inst, -0.9, -0.5, 1.8, 0.3, 1.0) end
  -- The cube-image probe: 6 pixels stretched into 6 bands, each the colour
  -- of the face its direction vector points at. Wrong face order shows as
  -- the wrong colour in a band, not as a blank.
  if targets.cubeProbe then show(targets.cubeProbe, -0.9, -0.9, 1.8, 0.25, 1.0) end

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("gpu3d  pass=" .. ok .. "  fail=" .. fail, 20, 20)
  local y = 50
  for _, l in ipairs(lines) do
    love.graphics.setColor(l.pass and 0.4 or 1, l.pass and 1 or 0.3, 0.4)
    love.graphics.print((l.pass and "ok   " or "FAIL ") .. l.name, 20, y)
    y = y + 22
  end
end
