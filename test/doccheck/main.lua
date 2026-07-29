-- Every code snippet from README.md / docs/api.md, run for real.
local ok = 0
local function T(name, fn) local good, err = pcall(fn)
  if good then ok = ok + 1 else love.log("DOC FAIL "..name..": "..tostring(err)) end end

function love.load()
  -- README quick-start snippet
  T("readme-basic", function()
    love.graphics.setColor(0.4, 0.8, 1)
    love.graphics.circle("fill", 100, 100, 36)
    love.graphics.print("hello from lua!", 40, 40)
  end)
  -- api.md: graphics objects
  T("objects", function()
    local cv = love.graphics.newCanvas(64, 64)
    love.graphics.setCanvas(cv); love.graphics.setCanvas()
    local f1 = love.graphics.newFont(24); f1:getHeight(); f1:getWidth("x")
    love.graphics.draw(cv, 0, 0)
  end)
  -- api.md: transforms
  T("transforms", function()
    love.graphics.push(); love.graphics.translate(1,2); love.graphics.rotate(0.5)
    love.graphics.scale(2); love.graphics.pop(); love.graphics.origin()
  end)
  -- api.md: pad
  T("pad", function()
    love.pad.isDown("a"); love.pad.isDown(2,"a"); love.pad.wasPressed("start")
    love.pad.wasReleased("b"); love.pad.axis("leftx"); love.pad.axis(3,"lefty")
  end)
  -- api.md: rumble
  T("rumble", function()
    love.pad.hasVibration(); love.pad.hasVibration(2)
    love.pad.setVibration(0.5, 0.25, 0.4)
    love.pad.setVibration(2, 1, 0, 0.4)
    love.pad.setVibration()
    love.pad.stopVibration(2)
    love.pad.getVibration()
    local j = love.joystick.getJoysticks()[1]
    j:isVibrationSupported(); j:setVibration(0.5, 0.5, 0.2); j:getVibration()
  end)
  -- api.md: keyboard/joystick
  T("keyboard", function()
    love.keyboard.isDown("left","a")
    for _,j in ipairs(love.joystick.getJoysticks()) do
      j:isGamepadDown("a"); j:getGamepadAxis("leftx") end
  end)
  -- api.md: mouse
  T("mouse", function()
    love.mouse.getPosition(); love.mouse.getX(); love.mouse.getY(); love.mouse.isDown(1,2)
  end)
  -- api.md: math / timer
  T("math-timer", function()
    love.math.random(); love.math.random(6); love.math.random(1,6)
    love.math.noise(1,2); love.math.setRandomSeed()
    love.timer.getTime(); love.timer.getDelta(); love.timer.getFPS()
  end)
  -- api.md: filesystem
  T("filesystem", function()
    love.filesystem.getInfo("main.lua"); love.filesystem.exists("main.lua")
    love.filesystem.write(nil, "high=1200"); love.filesystem.load_save()
  end)
  -- api.md + README: physics / wf
  T("physics", function()
    love.physics.setMeter(32); love.physics.getMeter(); love.physics.stats()
    local w = wf.newWorld(0, 900)
    w:addCollisionClass("Player", { ignores = { "Enemy" } })
    w:addCollisionClass("Enemy")
    local p = w:newBSGRectangleCollider(10, 10, 40, 60, 8)
    p:setCollisionClass("Player"); p:setFixedRotation(true)
    p:applyLinearImpulse(0, -400); p:applyForce(1,1)
    p:getPosition(); p:getX(); p:getY(); p:setPosition(5,5); p:setX(5); p:setY(5)
    p:getLinearVelocity(); p:setLinearVelocity(0,0)
    p:setType("dynamic"); p:setLinearDamping(1); p:setGravityScale(1); p:setBullet(false)
    p:getAngle(); p:setAngle(0); p:getMass(); p:setObject({}); p:getObject()
    p.body:getPosition()                       -- the windfield body proxy
    w:newCircleCollider(50,50,10); w:newPolygonCollider({0,0, 10,0, 10,10})
    w:newLineCollider(0,0,10,10)
    w:update(1/60)
    p:enter("Enemy"); p:exit("Enemy"); p:getEnterCollisionData("Enemy")
    w:queryCircleArea(0,0,50); w:queryRectangleArea(0,0,10,10)
    w:queryPolygonArea({0,0, 10,0, 10,10})
    w:draw(150); w:getColliderCount()
    p:destroy(); w:destroy()
  end)
  -- api.md: shaders.
  --
  -- This cart runs on the CPU comparator, which has no GL, and api.md says
  -- newShader FAILS there rather than pretending. So the documented contract
  -- has both branches and this block asserts whichever one applies -- which
  -- is stronger than skipping: a build that silently accepted a shader with
  -- no GL would fail here.
  T("shaders", function()
    local ok_, res = pcall(love.graphics.newShader, [[
      vec4 effect(vec4 color, Image tex, vec2 texture_coords, vec2 screen_coords) {
        vec4 px = Texel(tex, texture_coords) * color;
        return vec4(1.0 - px.rgb, px.a);
      }
    ]])
    if ok_ then
      -- a GL host: the whole documented surface must be there
      assert(res:type() == "Shader", "newShader returned a non-Shader")
      res:send("u_time", 0.5)
      res:send("u_tint", { 0.2, 1.0, 0.4 })
      res:send("u_on", true)
      -- hasUniform must be READ-ONLY. Probing by writing would zero the
      -- uniform, so this asserts a send still reports success afterwards
      -- (a write-probe implementation passes this too, which is why the
      -- real gate is that hasUniform is a distinct C entry point -- but a
      -- name that does not exist must still answer false).
      assert(res:hasUniform("u_nope") == false,
             "hasUniform claimed a uniform that was never declared")
      love.graphics.setShader(res)
      assert(love.graphics.getShader() == res, "getShader did not report the bound shader")
      love.graphics.setShader()
      assert(love.graphics.getShader() == nil, "setShader() did not clear")
      res:release()
      -- vertex-only: newShader(nil, vertexcode) keeps the default fragment
      -- stage. This is the form that once dropped its argument entirely.
      local vs = love.graphics.newShader(nil, [[
        vec4 position(mat4 transform_projection, vec4 vertex_position) {
          return transform_projection * vertex_position;
        }
      ]])
      assert(vs:type() == "Shader", "vertex-only newShader returned a non-Shader")
      love.graphics.setShader(vs)
      love.graphics.setShader()
    else
      -- a host with no GL: the failure is required to be loud and to say why
      assert(tostring(res):find("newShader"),
             "newShader failed without naming itself: " .. tostring(res))
    end
  end)
  -- api.md: meshes.
  --
  -- Same two-branch shape as the shader block above, and for the same
  -- reason: api.md documents newMesh as FAILING on a host with no GL rather
  -- than pretending, so this cart (which runs on the CPU comparator) asserts
  -- whichever branch applies. A build that silently handed back a mesh with
  -- no GL to draw it would fail here.
  T("meshes", function()
    local ok_, res = pcall(love.graphics.newMesh, {
      {   0,   0,  0, 0,  1, 0, 0, 1 },
      { 180,   0,  1, 0,  0, 1, 0, 1 },
      { 180, 180,  1, 1,  0, 0, 1, 1 },
      {   0, 180,  0, 1,  1, 1, 1, 1 },
    }, "fan")
    if ok_ then
      -- a GL host: the whole documented surface must be there
      assert(res:type() == "Mesh", "newMesh returned a non-Mesh")
      assert(res:getVertexCount() == 4, "getVertexCount wrong")
      assert(res:getDrawMode() == "fan", "getDrawMode wrong")
      res:setVertex(1, 1, 2, 0, 0, 1, 1, 1, 1)
      res:setVertex(2, { 3, 4, 0, 0, 1, 1, 1, 1 })
      local x, y = res:getVertex(1)
      assert(x == 1 and y == 2, "setVertex/getVertex did not round-trip")
      res:setVertices({ { 5, 6, 0, 0, 1, 1, 1, 1 } }, 3)
      res:setVertexMap({ 1, 2, 3, 1, 3, 4 })
      assert(#res:getVertexMap() == 6, "getVertexMap wrong")
      res:setVertexMap(nil)
      res:setDrawRange(1, 3)
      assert(select(1, res:getDrawRange()) == 1, "getDrawRange wrong")
      res:setDrawRange()
      local cv = love.graphics.newCanvas(32, 32)
      res:setTexture(cv)
      assert(res:getTexture() == cv, "getTexture did not report setTexture")
      res:setTexture(nil)
      love.graphics.draw(res, 0, 0)
      -- a count-built mesh defaults to opaque white, as documented
      local m2 = love.graphics.newMesh(3, "triangles")
      local _, _, _, _, r = m2:getVertex(1)
      assert(r == 1, "newMesh(count) did not default to white")
      m2:release()
      res:release()
      -- the documented refusals
      assert(not pcall(love.graphics.newMesh, { { 0, 0 } }, "points"),
             "\"points\" mode should be refused")
      assert(not pcall(love.graphics.newMesh,
             { { "VertexPosition", "float", 2 } }, 4, "fan"),
             "a custom vertex format should be refused")
    else
      -- a host with no GL: the failure is required to be loud and to say why
      assert(tostring(res):find("newMesh"),
             "newMesh failed without naming itself: " .. tostring(res))
    end
  end)
  -- api.md: debug + logging
  T("debug", function() love.log("x", 1); love.debugValue(0, 5); love.mark(7) end)
  -- api.md: window/system/event
  T("window", function()
    love.window.getWidth(); love.window.getHeight(); love.window.getDimensions()
    love.window.setTitle("x"); love.window.setMode(1,1)
    love.system.getOS(); love.event.quit()
  end)
  -- porting guide: 5.1 compat layer
  T("lua51-compat", function()
    assert(unpack({1,2})==1); assert(table.getn({1,2})==2)
    assert(loadstring("return 1")()==1); assert(math.atan2(1,1))
    assert(math.mod(5,2)==1); assert(math.pow(2,3)==8)
    local f=function() return 1 end; setfenv(f,{}); getfenv(f)
    assert(os.time()); assert(os.clock()); assert(os.date())
  end)
  love.log(("DOCCHECK %d blocks OK"):format(ok))
  love.debugValue(0, ok)
end
function love.draw() end
