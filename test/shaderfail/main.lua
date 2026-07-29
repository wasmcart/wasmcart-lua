-- shaderfail/main.lua - the control that MUST fail.
--
-- A shader feature that only ever reports success is indistinguishable from
-- one that cannot detect failure at all. This cart feeds newShader four
-- shaders that are each broken in a different way and asserts that every one
-- of them is REFUSED, with the reason reaching the cart log.
--
-- It draws its verdict, so the screenshot is the evidence: a green PASS line
-- per case means the engine caught it, a red FAIL means a broken shader was
-- accepted and would have rendered silently wrong.

local cases = {}

local function check(label, src, want)
  local ok, msgOrShader = pcall(love.graphics.newShader, src)
  cases[#cases + 1] = {
    label = label,
    -- refused == the call raised. Accepting a broken shader is the failure.
    passed = (ok == false),
    detail = ok and "ACCEPTED (should not have been)" or tostring(msgOrShader),
    want = want,
  }
end

function love.load()
  love.graphics.setBackgroundColor(0.06, 0.06, 0.1)

  -- 1. a plain GLSL syntax error: the driver must report it, not black-screen
  check("syntax error", [[
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
      return vec4(1.0, 0.0, 0.0   // no closing paren, no semicolon
    }
  ]], "driver compile error")

  -- 2. desktop GLSL: our surface is WebGL2 / GLES 3.0 only
  check("own #version", [[
    #version 330 core
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) { return color; }
  ]], "rejected by name")

  -- 3. GLSL ES 1.00 spelling, the single most common copy-paste mistake
  check("gl_FragColor", [[
    vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
      gl_FragColor = color;
      return color;
    }
  ]], "rejected by name")

  -- 4. compiles fine but has no effect(), so the LINK must fail
  check("no effect()", [[
    vec4 notTheRightName(vec4 color) { return color; }
  ]], "link error")
end

function love.draw()
  local allPass = true
  -- machine-readable verdict for test/run.js: slot 0 = how many broken
  -- shaders were WRONGLY accepted, slot 1 = how many cases ran at all
  local accepted = 0
  for _, c in ipairs(cases) do if not c.passed then accepted = accepted + 1 end end
  love.debugValue(0, accepted)
  love.debugValue(1, #cases)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("newShader must REFUSE each of these:", 40, 40)
  for i, c in ipairs(cases) do
    if not c.passed then allPass = false end
    local y = 90 + (i - 1) * 90
    if c.passed then
      love.graphics.setColor(0.2, 1, 0.4)
      love.graphics.print("PASS  " .. c.label .. "  (" .. c.want .. ")", 40, y)
    else
      love.graphics.setColor(1, 0.25, 0.2)
      love.graphics.print("FAIL  " .. c.label .. "  " .. c.detail, 40, y)
    end
  end

  -- a big unmissable verdict bar
  if allPass then
    love.graphics.setColor(0.1, 0.7, 0.25)
    love.graphics.rectangle("fill", 40, 500, 700, 70)
    love.graphics.setColor(1, 1, 1)
    love.graphics.print("ALL 4 BROKEN SHADERS WERE REFUSED", 60, 525)
  else
    love.graphics.setColor(0.8, 0.1, 0.1)
    love.graphics.rectangle("fill", 40, 500, 700, 70)
    love.graphics.setColor(1, 1, 1)
    love.graphics.print("A BROKEN SHADER WAS ACCEPTED", 60, 525)
  end

  -- Proof the SAME cart can still make a shader that works, so "everything
  -- fails" cannot pass this test either.
  if not love.__good then
    -- channel rotation: r->b, so a red vertex colour comes out BLUE
    love.__good = love.graphics.newShader [[
      vec4 effect(vec4 color, Image tex, vec2 tc, vec2 sc) {
        return vec4(color.g, color.b, color.r, color.a);
      }
    ]]
  end
  love.graphics.setShader(love.__good)
  love.graphics.setColor(1, 0, 0)   -- red in; blue out iff the shader ran
  love.graphics.rectangle("fill", 40, 610, 700, 70)
  love.graphics.setShader()
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("this bar is drawn RED; a working shader makes it BLUE", 40, 690)
end
