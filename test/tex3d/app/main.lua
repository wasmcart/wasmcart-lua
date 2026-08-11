-- TEXTURE MAPPING ON A 3D MODEL.
--
-- The quad test proved uv orientation. It could not prove texture mapping,
-- because a screen-aligned quad has none of the things that make texturing
-- on a model hard:
--
--   * PERSPECTIVE-CORRECT INTERPOLATION. On a flat quad facing the camera,
--     linear and perspective-correct interpolation give the same answer. On
--     a surface at an angle they do not, and the difference is the classic
--     PS1-style texture swim -- straight lines on the surface bending.
--   * MINIFICATION. A model's far side packs many texels into few pixels.
--     Without mipmaps that aliases into noise.
--   * A REAL UV LAYOUT. Suzanne's uvs wrap around a curved surface with
--     seams, not a single 0..1 rect.
--
-- So this draws the actual monkey, with the actual uvs from its .obj,
-- wearing a grid whose straight lines make any of those failures visible:
-- bent grid lines = no perspective correction, shimmering = no mipmaps,
-- torn/garbled cells = wrong uv attribute.
--
-- Left: textured. Right: the same model showing its raw uv as colour, which
-- proves the uv attribute reached the shader with the right values at all.

local dream = require("3DreamEngine.init")
dream:init()
local monkey = dream:loadObject("examples/monkey/object")

local texShader, uvShader, tex, mesh
local t = 0

local VERT = [[
  extern mat4 vp;
  extern vec3 offset;
  vec4 position(mat4 tp, vec4 v) {
    vec3 n = VertexNormal;
    return vp * vec4(v.xyz + offset, 1.0);
  }
]]

function love.load()
  tex = love.graphics.newImage("uvgrid.png")
  tex:setWrap("repeat", "repeat")

  texShader = love.graphics.newShader([[
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      vec3 n = normalize(VertexNormal);
      float l = 0.45 + 0.55 * max(dot(n, normalize(vec3(0.3, 0.7, 0.6))), 0.0);
      return vec4(Texel(t, uv).rgb * l, 1.0);
    }
  ]], VERT)

  -- Raw uv as colour: red = u, green = v. A correct layout is a smooth
  -- two-axis gradient over the surface; a dropped attribute is flat black.
  uvShader = love.graphics.newShader([[
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      return vec4(uv.x, uv.y, 0.25, 1.0);
    }
  ]], VERT)

  mesh = monkey.meshes.Suzanne:getMesh()
  mesh:setTexture(tex)
  print("TEX3D mesh=" .. tostring(mesh) .. " tex=" .. tostring(tex.id))
end

function love.update(dt) t = t + dt end

local function viewProj(aspect)
  local f, n, fa = 1.1, 0.1, 100
  local d = 2.6
  -- An ANGLED view, deliberately: perspective error is invisible head-on.
  local ex, ey, ez = math.cos(t * 0.4) * d, 1.6, math.sin(t * 0.4) * d
  local zx, zy, zz = ex, ey, ez
  local zl = math.sqrt(zx*zx + zy*zy + zz*zz); zx, zy, zz = zx/zl, zy/zl, zz/zl
  local xx, xy, xz = 1*zz - 0*zy, 0*zx - 0*zz, 0*zy - 1*zx
  local xl = math.sqrt(xx*xx + xy*xy + xz*xz); xx, xy, xz = xx/xl, xy/xl, xz/xl
  local yx, yy, yz = zy*xz - zz*xy, zz*xx - zx*xz, zx*xy - zy*xx
  local view = {
    xx, xy, xz, -(xx*ex + xy*ey + xz*ez),
    yx, yy, yz, -(yx*ex + yy*ey + yz*ez),
    zx, zy, zz, -(zx*ex + zy*ey + zz*ez),
    0, 0, 0, 1,
  }
  local p = { f/aspect,0,0,0, 0,f,0,0,
              0,0,-(fa+n)/(fa-n), -2*fa*n/(fa-n), 0,0,-1,0 }
  local m = {}
  for r = 0, 3 do for c = 0, 3 do
    local s = 0
    for k = 0, 3 do s = s + p[r*4+k+1] * view[k*4+c+1] end
    m[r*4+c+1] = s
  end end
  return m
end

function love.draw()
  love.graphics.clear(0.04, 0.05, 0.09, 1)
  local m = viewProj(16 / 9)

  love.graphics.setDepthMode("lequal", true)
  love.graphics.setMeshCullMode("back")

  love.graphics.setShader(texShader)
  texShader:send("vp", m)
  texShader:send("offset", { 0, 0, 0 })
  love.graphics.draw(mesh)

  love.graphics.setShader()
  love.graphics.setDepthMode()
  love.graphics.setMeshCullMode("none")

  love.graphics.setColor(1, 1, 1)
  love.graphics.print("textured (grid must stay straight)", 150, 650)
  love.graphics.print("raw uv as colour", 830, 650)
end
