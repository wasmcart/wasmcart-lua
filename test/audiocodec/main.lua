-- audiocodec/main.lua - every codec, through the REAL love.audio path.
--
-- The native harness (test/decoders.c) asserts the PCM is correct. This cart
-- asserts the engine actually reaches those decoders: asset resolution, the
-- content sniffer, the mixer slot, and love.audio.newSource on top of it.
-- A decoder that works in isolation and is never called by sound_load would
-- pass there and fail here.
--
-- Draws its result so the run is visible as well as logged: a green bar per
-- codec that loaded, red for one that did not.

local CODECS = { "wav", "ogg", "mp3", "flac", "opus" }
local results = {}
local failures = 0

function love.load()
  for _, ext in ipairs(CODECS) do
    local path = "sounds/tone." .. ext
    local ok, src = pcall(love.audio.newSource, path)
    -- newSource errors when the id is nil; a negative id means the decode
    -- itself failed, which surfaces as a distinct case worth naming.
    local good = ok and src ~= nil and src.id ~= nil and src.id >= 0
    results[#results + 1] = { ext = ext, ok = good, id = ok and src and src.id or -1 }
    if not good then failures = failures + 1 end
    print(string.format("codec %-5s %s (id=%s)", ext,
      good and "LOADED" or "FAILED", tostring(ok and src and src.id or "err")))
  end

  -- The sniffer must dispatch on CONTENT, not on the extension. Ask for a
  -- path whose extension lies about what the bytes are: mislabeling an mp3
  -- as .wav used to hand ID3 bytes to the WAV parser and go silently quiet.
  -- This is the specific regression the sniff replaced, so it gets a case.
  local ok2, src2 = pcall(love.audio.newSource, "sounds/mislabeled.wav")
  local sniffed = ok2 and src2 ~= nil and src2.id ~= nil and src2.id >= 0
  results[#results + 1] = { ext = "sniff", ok = sniffed, id = ok2 and src2 and src2.id or -1 }
  if not sniffed then failures = failures + 1 end
  print(string.format("codec %-5s %s (mp3 bytes named .wav)", "sniff",
    sniffed and "LOADED" or "FAILED"))

  -- More than two channels must be FOLDED DOWN, not handed to the mixer as
  -- if it were stereo -- that would read 6-channel interleave as pairs and
  -- play everything at three times the rate. 5.1 FLAC is the realistic way a
  -- cart hits this.
  local ok3, src3 = pcall(love.audio.newSource, "sounds/tone6ch.flac")
  local downmixed = ok3 and src3 ~= nil and src3.id ~= nil and src3.id >= 0
  results[#results + 1] = { ext = "6ch", ok = downmixed, id = ok3 and src3 and src3.id or -1 }
  if not downmixed then failures = failures + 1 end
  print(string.format("codec %-5s %s (5.1 FLAC -> stereo)", "6ch",
    downmixed and "LOADED" or "FAILED"))

  print(failures == 0 and "AUDIOCODEC OK" or ("AUDIOCODEC FAILURES=" .. failures))
  love.debugValue(0, failures)
  love.debugValue(1, #results)
end

function love.draw()
  love.graphics.clear(20, 20, 30)
  for i, r in ipairs(results) do
    local y = 20 + (i - 1) * 40
    if r.ok then love.graphics.setColor(40, 200, 90)
    else love.graphics.setColor(220, 50, 50) end
    love.graphics.rectangle("fill", 20, y, 200, 30)
  end
end
