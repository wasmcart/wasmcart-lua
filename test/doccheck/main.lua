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
