-- Stores all clickable buttons on the main menu
-- By index: X, Y, Width, Height, Message
buttons = {}
buttons[1] = {376, 380, 360, 72, "New Game"}
buttons[2] = {376, 476, 360, 72, "Continue"}

-- Dimensions and offset for the small corner buttons (sound and GitHub)
local smSize = 72
local smOffset = 14

-- Place buttons 3 and 4 in the left and right corners respectively
buttons[3] = {smOffset, gameHeight - smSize - smOffset, smSize, smSize, ".sound"}
buttons[4] = {gameWidth - smSize - smOffset, gameHeight - smSize - smOffset, smSize, smSize, ".github"}

-- This value stores the message displayed at the bottom of the menu
buttons.message = ""

-- GAMEPAD NAVIGATION (added for the wasmcart port).
--
-- Upstream Cavern is a mouse game: the menu highlights on hover and acts on
-- click, with no notion of a "selected" item. wasmcart is gamepad-first --
-- the host synthesizes a pad from the keyboard when no physical pad is
-- present, so love.pad is the input every cart can rely on and a mouse is
-- the thing that may not exist. A pad host cannot produce a hover, so this
-- menu was literally unstartable there.
--
-- The pad therefore OWNS the selection. The mouse is kept as an optional
-- convenience on hosts that have one: hovering moves the pad's cursor rather
-- than running a second, parallel notion of focus.
buttons.sel = 1

-- Only the two big menu entries are reachable with the pad. The corner
-- sound/GitHub buttons stay mouse-only on purpose: opening a URL is
-- meaningless on a console host, and a pad user should not be able to land
-- on it by accident.
local PAD_ITEMS = 2

-- Per-frame pad handling for the menu. Called from love.update.
function buttons:padUpdate()

  if gameState.room ~= "rmMainMenu" then return end

  local move = 0
  if love.pad.wasPressed(1, "up")   then move = -1 end
  if love.pad.wasPressed(1, "down") then move =  1 end
  -- analog stick, for pads whose d-pad is not wired
  if move == 0 then
    local ly = love.pad.axis(1, "lefty")
    if ly < -0.6 and not self.stickHeld then move = -1; self.stickHeld = true
    elseif ly > 0.6 and not self.stickHeld then move = 1; self.stickHeld = true
    elseif math.abs(ly) < 0.3 then self.stickHeld = false end
  end

  if move ~= 0 then
    self.sel = self.sel + move
    if self.sel < 1 then self.sel = PAD_ITEMS end
    if self.sel > PAD_ITEMS then self.sel = 1 end
    soundManager:play("click")
  end

  -- Confirm. Accept BOTH face buttons plus start: which physical button is
  -- "confirm" differs by platform convention (and by how a given host maps
  -- its pad), and guessing wrong strands the player on the title screen with
  -- no way into the game at all. Being permissive here costs nothing.
  if love.pad.wasPressed(1, "a") or love.pad.wasPressed(1, "b")
     or love.pad.wasPressed(1, "start") then
    self:activate(self.sel)
  end

end

-- On hosts that HAVE a mouse, hovering just moves the pad cursor, so there
-- is only ever one selection. Returns true if the mouse claimed a button.
function buttons:syncMouse()
  for i = 1, PAD_ITEMS do
    if self:mouseCheck(self[i]) then
      self.sel = i
      return true
    end
  end
  return false
end

-- This function draws everything on the Main Menu
function menuDraw()

  if gameState.room == "rmMainMenu" then

    love.graphics.setFont(fonts.menu.title)
    love.graphics.setColor(1, 1, 1, 1)
    love.graphics.printf("CAVERN", 0, 140 * scale, gameWidth * scale, "center")

    -- Start message off as nothing, will be updated if hovering over a button
    buttons.message = ""

    for i,b in ipairs(buttons) do

      -- Get attributes stored for the current button
      local bX = b[1] * scale;
      local bY = b[2] * scale;
      local bW = b[3] * scale;
      local bH = b[4] * scale;
      local bText = b[5];

      if buttons:isFocused(b, i) then -- the current selection

        -- Button border
        -- love.graphics.setColor(0.384, 0.604, 0.475) -- enemy color
        love.graphics.setColor(1, 1, 1) -- white
        love.graphics.setLineWidth(6)
        love.graphics.rectangle("line", bX, bY, bW, bH)

        -- Update the button message at the bottom of the screen
        if bText == "New Game" then
          buttons.message = "Start a new game - erases old save file"
        elseif bText == "Continue" then
          buttons.message = "Continue from where you left off"
        elseif bText == ".sound" then
          buttons.message = "Turn music and sound effects on or off"
        elseif bText == ".github" then
          buttons.message = "View the code on GitHub"
        end

      end

      love.graphics.setColor(1, 1, 1, 1)
      love.graphics.setFont(fonts.menu.button)

      if bText == ".sound" then
        if not soundOn then
          love.graphics.setColor(0.35, 0.35, 0.35, 0.5)
        end
        love.graphics.draw(sprites.ui.sound, bX + 15 * scale, bY + 9 * scale, 0, scale, scale)
      elseif bText == ".github" then
        love.graphics.draw(sprites.ui.github, bX + 9 * scale, bY + 8 * scale, 0, scale, scale)
      else
        love.graphics.printf(bText, bX, bY + 8 * scale, bW, "center")
      end

    end

  end

  love.graphics.setColor(1, 1, 1, 1)
  love.graphics.setFont(fonts.menu.message)
  love.graphics.printf(buttons.message, 0, love.graphics.getHeight() - 80, love.graphics.getWidth(), "center")

end

-- Is this button the one the player is currently on? The two pad-navigable
-- entries follow buttons.sel; the mouse-only corner buttons still light up
-- on hover, on hosts that have a mouse.
function buttons:isFocused(b, i)
  if i and i <= PAD_ITEMS then return self.sel == i end
  return self:mouseCheck(b)
end

-- Check if the mouse is inside the passed button
function buttons:mouseCheck(b)

  -- Get mouse coordinates
  local mx, my = love.mouse.getPosition()

  -- Get attributes stored for the passed button
  local bX = b[1] * scale;
  local bY = b[2] * scale;
  local bW = b[3] * scale;
  local bH = b[4] * scale;

  -- Compare coordinates to see if mouse is inside button
  if mx > bX and mx < bX+bW and my > bY and my < bY+bH then
    return true
  end

  return false

end

-- Called whenever the left mouse button is clicked
-- Checks if it clicked on a button, and does what that
-- button needs to do
function buttons:click()

  for i,b in ipairs(self) do

    -- If the mouse is on the current button...
    if buttons:mouseCheck(b) then
      self:activate(i)
    end

  end

end

-- The actual effect of button `i`, independent of which device chose it.
function buttons:activate(i)

  do

      if i == 1 then -- New Game button

        -- This is the state for new game sequence
        intro.state = 1
        intro.timer = 1
        buttons.message = ""
        soundManager:musicFade()
        changeToMap("rmIntro")

      elseif i == 2 then -- Continue button

        -- This is the state for intro's load sequence
        intro.state = 100
        intro.timer = 1.5
        buttons.message = ""
        soundManager:musicFade()
        changeToMap("rmIntro")

      elseif i == 3 then -- Sound button

        -- Toggle sound to be on/off
        soundOn = not soundOn
        if soundOn then
          soundManager:startMusic("menu")
        else
          soundManager:musicFade()
        end

      elseif i == 4 then -- GitHub button

        -- Open the GitHub page for this game!
        love.system.openURL("https://github.com/kyleschaub/cavern")

      end

      soundManager:play("click")

  end

end
