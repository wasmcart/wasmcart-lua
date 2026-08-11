-- Draw 3Dream's monkey mesh with MY OWN simple shader, straight to the
-- screen. If the monkey appears, the geometry and the generic vertex format
-- are right and the problem is inside 3Dream's g-buffer plumbing. If it does
-- not, the mesh data itself is wrong.
local dream = require("3DreamEngine.init")
dream:init()
local monkey = dream:loadObject("examples/monkey/object")

local shader, mesh
function love.load()
  shader = love.graphics.newShader([[
    vec4 effect(vec4 c, Image t, vec2 uv, vec2 sc) {
      // shade by normal so the shape is unmistakable
      vec3 n = normalize(VertexNormal);
      return vec4(n * 0.5 + 0.5, 1.0);
    }
  ]], [[
    extern mat4 mvp;
    vec4 position(mat4 tp, vec4 vp) {
      return mvp * vec4(vp.xyz, 1.0);
    }
  ]])
  mesh = monkey.meshes.Suzanne:getMesh()
  print("MESH=" .. tostring(mesh) .. " type=" .. tostring(mesh and mesh:type()))
end

function love.draw()
  love.graphics.clear(0.05, 0.05, 0.12, 1)
  -- a hand-built perspective * view matrix, ROW-major (LOVE's convention)
  local f, aspect, n, fa = 1.2, 16/9, 0.1, 100
  local t = love.timer.getTime()
  local cx, cz = math.cos(t) * 3.2, math.sin(t) * 3.2
  -- look from (cx,1.5,cz) at origin
  local ex, ey, ez = cx, 1.5, cz
  local zx, zy, zz = ex, ey, ez
  local zl = math.sqrt(zx*zx+zy*zy+zz*zz); zx,zy,zz = zx/zl, zy/zl, zz/zl
  local ux, uy, uz = 0, 1, 0
  local xx, xy, xz = uy*zz-uz*zy, uz*zx-ux*zz, ux*zy-uy*zx
  local xl = math.sqrt(xx*xx+xy*xy+xz*xz); xx,xy,xz = xx/xl, xy/xl, xz/xl
  local yx, yy, yz = zy*xz-zz*xy, zz*xx-zx*xz, zx*xy-zy*xx
  local view = {
    xx, xy, xz, -(xx*ex+xy*ey+xz*ez),
    yx, yy, yz, -(yx*ex+yy*ey+yz*ez),
    zx, zy, zz, -(zx*ex+zy*ey+zz*ez),
    0, 0, 0, 1,
  }
  local p = {
    f/aspect,0,0,0,  0,f,0,0,
    0,0,-(fa+n)/(fa-n), -2*fa*n/(fa-n),
    0,0,-1,0,
  }
  -- mvp = p * view  (row-major multiply)
  local m = {}
  for r = 0, 3 do for c = 0, 3 do
    local s = 0
    for k = 0, 3 do s = s + p[r*4+k+1] * view[k*4+c+1] end
    m[r*4+c+1] = s
  end end

  love.graphics.setDepthMode("lequal", true)
  love.graphics.setShader(shader)
  shader:send("mvp", m)
  love.graphics.draw(mesh)
  love.graphics.setShader()
  love.graphics.setDepthMode()
end
