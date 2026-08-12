# wasmcart-lua: LOVE API status

**Generated, not hand-maintained.** Every row comes from walking the
`love` table of a running engine, so this file cannot claim a function
that is not actually exported.

To regenerate:

```sh
# 1. run the enumerator as a cart and capture its log
#    (tools/apistatus/enumerate.lua prints WCLUA_API_BEGIN ... END)
# 2. feed the dump in:
node tools/apistatus/gen-status.mjs api-dump.txt
```

**186 of 320 LOVE functions implemented (58%).**

A caveat worth stating plainly: this table measures PRESENCE. A
function can be exported and still be wrong. `test/apiconform/`
exists for that half -- it asserts on values (round-trips, known-answer
maths, conserved areas) rather than on whether a name resolves.

The denominator is libretro's [lutro-status](https://github.com/libretro/lutro-status)
list, so this percentage and Lutro's are directly comparable.

## Coverage by module

| module | implemented | total | coverage |
|---|---:|---:|---:|
| `love` | 0 | 4 | 0% |
| `love.audio` | 3 | 26 | 12% |
| `love.data` | 9 | 10 | 90% |
| `love.event` | 5 | 6 | 83% |
| `love.filesystem` | 14 | 31 | 45% |
| `love.font` | 1 | 5 | 20% |
| `love.graphics` | 78 | 105 | 74% |
| `love.image` | 3 | 3 | 100% |
| `love.joystick` | 2 | 6 | 33% |
| `love.keyboard` | 9 | 9 | 100% |
| `love.math` | 11 | 16 | 69% |
| `love.mouse` | 8 | 18 | 44% |
| `love.physics` | 3 | 22 | 14% |
| `love.sound` | 2 | 2 | 100% |
| `love.system` | 7 | 8 | 88% |
| `love.thread` | 3 | 3 | 100% |
| `love.timer` | 5 | 6 | 83% |
| `love.touch` | 3 | 3 | 100% |
| `love.video` | 0 | 1 | 0% |
| `love.window` | 20 | 36 | 56% |

## Function by function

### `love` — 0/4

| | function |
|---|---|
| :white_medium_square: | `love.getVersion` |
| :white_medium_square: | `love.hasDeprecationOutput` |
| :white_medium_square: | `love.isVersionCompatible` |
| :white_medium_square: | `love.setDeprecationOutput` |

### `love.audio` — 3/26

| | function |
|---|---|
| :white_medium_square: | `love.audio.getActiveEffects` |
| :white_medium_square: | `love.audio.getActiveSourceCount` |
| :white_medium_square: | `love.audio.getDistanceModel` |
| :white_medium_square: | `love.audio.getDopplerScale` |
| :white_medium_square: | `love.audio.getEffect` |
| :white_medium_square: | `love.audio.getMaxSceneEffects` |
| :white_medium_square: | `love.audio.getMaxSourceEffects` |
| :white_medium_square: | `love.audio.getOrientation` |
| :white_medium_square: | `love.audio.getPosition` |
| :white_medium_square: | `love.audio.getRecordingDevices` |
| :white_medium_square: | `love.audio.getVelocity` |
| :white_medium_square: | `love.audio.getVolume` |
| :white_medium_square: | `love.audio.isEffectsSupported` |
| :white_medium_square: | `love.audio.newQueueableSource` |
| :white_check_mark: | `love.audio.newSource` |
| :white_medium_square: | `love.audio.pause` |
| :white_check_mark: | `love.audio.play` |
| :white_medium_square: | `love.audio.setDistanceModel` |
| :white_medium_square: | `love.audio.setDopplerScale` |
| :white_medium_square: | `love.audio.setEffect` |
| :white_medium_square: | `love.audio.setMixWithSystem` |
| :white_medium_square: | `love.audio.setOrientation` |
| :white_medium_square: | `love.audio.setPosition` |
| :white_medium_square: | `love.audio.setVelocity` |
| :white_medium_square: | `love.audio.setVolume` |
| :white_check_mark: | `love.audio.stop` |

### `love.data` — 9/10

| | function |
|---|---|
| :white_check_mark: | `love.data.compress` |
| :white_check_mark: | `love.data.decode` |
| :white_check_mark: | `love.data.decompress` |
| :white_check_mark: | `love.data.encode` |
| :white_check_mark: | `love.data.getPackedSize` |
| :white_check_mark: | `love.data.hash` |
| :white_check_mark: | `love.data.newByteData` |
| :white_medium_square: | `love.data.newDataView` |
| :white_check_mark: | `love.data.pack` |
| :white_check_mark: | `love.data.unpack` |

### `love.event` — 5/6

| | function |
|---|---|
| :white_check_mark: | `love.event.clear` |
| :white_check_mark: | `love.event.poll` |
| :white_check_mark: | `love.event.pump` |
| :white_check_mark: | `love.event.push` |
| :white_check_mark: | `love.event.quit` |
| :white_medium_square: | `love.event.wait` |

### `love.filesystem` — 14/31

| | function |
|---|---|
| :white_medium_square: | `love.filesystem.append` |
| :white_medium_square: | `love.filesystem.areSymlinksEnabled` |
| :white_check_mark: | `love.filesystem.createDirectory` |
| :white_medium_square: | `love.filesystem.getAppdataDirectory` |
| :white_medium_square: | `love.filesystem.getCRequirePath` |
| :white_check_mark: | `love.filesystem.getDirectoryItems` |
| :white_check_mark: | `love.filesystem.getIdentity` |
| :white_check_mark: | `love.filesystem.getInfo` |
| :white_check_mark: | `love.filesystem.getRealDirectory` |
| :white_medium_square: | `love.filesystem.getRequirePath` |
| :white_check_mark: | `love.filesystem.getSaveDirectory` |
| :white_medium_square: | `love.filesystem.getSource` |
| :white_medium_square: | `love.filesystem.getSourceBaseDirectory` |
| :white_medium_square: | `love.filesystem.getUserDirectory` |
| :white_medium_square: | `love.filesystem.getWorkingDirectory` |
| :white_medium_square: | `love.filesystem.init` |
| :white_medium_square: | `love.filesystem.isFused` |
| :white_check_mark: | `love.filesystem.lines` |
| :white_check_mark: | `love.filesystem.load` |
| :white_check_mark: | `love.filesystem.mount` |
| :white_check_mark: | `love.filesystem.newFile` |
| :white_medium_square: | `love.filesystem.newFileData` |
| :white_check_mark: | `love.filesystem.read` |
| :white_check_mark: | `love.filesystem.remove` |
| :white_medium_square: | `love.filesystem.setCRequirePath` |
| :white_check_mark: | `love.filesystem.setIdentity` |
| :white_medium_square: | `love.filesystem.setRequirePath` |
| :white_medium_square: | `love.filesystem.setSource` |
| :white_medium_square: | `love.filesystem.setSymlinksEnabled` |
| :white_medium_square: | `love.filesystem.unmount` |
| :white_check_mark: | `love.filesystem.write` |

### `love.font` — 1/5

| | function |
|---|---|
| :white_medium_square: | `love.font.newBMFontRasterizer` |
| :white_medium_square: | `love.font.newGlyphData` |
| :white_medium_square: | `love.font.newImageRasterizer` |
| :white_check_mark: | `love.font.newRasterizer` |
| :white_medium_square: | `love.font.newTrueTypeRasterizer` |

### `love.graphics` — 78/105

| | function |
|---|---|
| :white_medium_square: | `love.graphics.Canvas.generateMipmaps` |
| :white_medium_square: | `love.graphics.Canvas.getMSAA` |
| :white_medium_square: | `love.graphics.Canvas.getMipmapMode` |
| :white_medium_square: | `love.graphics.Canvas.newImageData` |
| :white_medium_square: | `love.graphics.Canvas.renderTo` |
| :white_medium_square: | `love.graphics.applyTransform` |
| :white_check_mark: | `love.graphics.arc` |
| :white_check_mark: | `love.graphics.captureScreenshot` |
| :white_check_mark: | `love.graphics.circle` |
| :white_check_mark: | `love.graphics.clear` |
| :white_medium_square: | `love.graphics.discard` |
| :white_check_mark: | `love.graphics.draw` |
| :white_check_mark: | `love.graphics.drawInstanced` |
| :white_medium_square: | `love.graphics.drawLayer` |
| :white_medium_square: | `love.graphics.ellipse` |
| :white_medium_square: | `love.graphics.flushBatch` |
| :white_check_mark: | `love.graphics.getBackgroundColor` |
| :white_check_mark: | `love.graphics.getBlendMode` |
| :white_check_mark: | `love.graphics.getCanvas` |
| :white_check_mark: | `love.graphics.getCanvasFormats` |
| :white_check_mark: | `love.graphics.getColor` |
| :white_check_mark: | `love.graphics.getColorMask` |
| :white_check_mark: | `love.graphics.getDPIScale` |
| :white_check_mark: | `love.graphics.getDefaultFilter` |
| :white_check_mark: | `love.graphics.getDepthMode` |
| :white_check_mark: | `love.graphics.getDimensions` |
| :white_check_mark: | `love.graphics.getFont` |
| :white_check_mark: | `love.graphics.getFrontFaceWinding` |
| :white_check_mark: | `love.graphics.getHeight` |
| :white_medium_square: | `love.graphics.getImageFormats` |
| :white_check_mark: | `love.graphics.getLineJoin` |
| :white_check_mark: | `love.graphics.getLineStyle` |
| :white_check_mark: | `love.graphics.getLineWidth` |
| :white_check_mark: | `love.graphics.getMeshCullMode` |
| :white_check_mark: | `love.graphics.getPixelDimensions` |
| :white_check_mark: | `love.graphics.getPixelHeight` |
| :white_check_mark: | `love.graphics.getPixelWidth` |
| :white_check_mark: | `love.graphics.getPointSize` |
| :white_check_mark: | `love.graphics.getRendererInfo` |
| :white_check_mark: | `love.graphics.getScissor` |
| :white_check_mark: | `love.graphics.getShader` |
| :white_check_mark: | `love.graphics.getStackDepth` |
| :white_check_mark: | `love.graphics.getStats` |
| :white_check_mark: | `love.graphics.getStencilTest` |
| :white_medium_square: | `love.graphics.getSupported` |
| :white_check_mark: | `love.graphics.getSystemLimits` |
| :white_medium_square: | `love.graphics.getTextureTypes` |
| :white_check_mark: | `love.graphics.getWidth` |
| :white_medium_square: | `love.graphics.intersectScissor` |
| :white_medium_square: | `love.graphics.inverseTransformPoint` |
| :white_check_mark: | `love.graphics.isActive` |
| :white_medium_square: | `love.graphics.isGammaCorrect` |
| :white_medium_square: | `love.graphics.isWireframe` |
| :white_check_mark: | `love.graphics.line` |
| :white_check_mark: | `love.graphics.newArrayImage` |
| :white_check_mark: | `love.graphics.newCanvas` |
| :white_check_mark: | `love.graphics.newCubeImage` |
| :white_check_mark: | `love.graphics.newFont` |
| :white_check_mark: | `love.graphics.newImage` |
| :white_medium_square: | `love.graphics.newImageFont` |
| :white_check_mark: | `love.graphics.newMesh` |
| :white_medium_square: | `love.graphics.newParticleSystem` |
| :white_check_mark: | `love.graphics.newQuad` |
| :white_check_mark: | `love.graphics.newShader` |
| :white_check_mark: | `love.graphics.newSpriteBatch` |
| :white_medium_square: | `love.graphics.newText` |
| :white_check_mark: | `love.graphics.newVideo` |
| :white_check_mark: | `love.graphics.newVolumeImage` |
| :white_check_mark: | `love.graphics.origin` |
| :white_check_mark: | `love.graphics.points` |
| :white_check_mark: | `love.graphics.polygon` |
| :white_check_mark: | `love.graphics.pop` |
| :white_check_mark: | `love.graphics.present` |
| :white_check_mark: | `love.graphics.print` |
| :white_check_mark: | `love.graphics.printf` |
| :white_check_mark: | `love.graphics.push` |
| :white_check_mark: | `love.graphics.rectangle` |
| :white_medium_square: | `love.graphics.replaceTransform` |
| :white_check_mark: | `love.graphics.reset` |
| :white_check_mark: | `love.graphics.rotate` |
| :white_check_mark: | `love.graphics.scale` |
| :white_check_mark: | `love.graphics.setBackgroundColor` |
| :white_check_mark: | `love.graphics.setBlendMode` |
| :white_check_mark: | `love.graphics.setCanvas` |
| :white_check_mark: | `love.graphics.setColor` |
| :white_check_mark: | `love.graphics.setColorMask` |
| :white_check_mark: | `love.graphics.setDefaultFilter` |
| :white_check_mark: | `love.graphics.setDepthMode` |
| :white_check_mark: | `love.graphics.setFont` |
| :white_check_mark: | `love.graphics.setFrontFaceWinding` |
| :white_check_mark: | `love.graphics.setLineJoin` |
| :white_check_mark: | `love.graphics.setLineStyle` |
| :white_check_mark: | `love.graphics.setLineWidth` |
| :white_check_mark: | `love.graphics.setMeshCullMode` |
| :white_medium_square: | `love.graphics.setNewFont` |
| :white_check_mark: | `love.graphics.setPointSize` |
| :white_check_mark: | `love.graphics.setScissor` |
| :white_check_mark: | `love.graphics.setShader` |
| :white_medium_square: | `love.graphics.setStencilTest` |
| :white_medium_square: | `love.graphics.setWireframe` |
| :white_medium_square: | `love.graphics.shear` |
| :white_medium_square: | `love.graphics.stencil` |
| :white_medium_square: | `love.graphics.transformPoint` |
| :white_check_mark: | `love.graphics.translate` |
| :white_check_mark: | `love.graphics.validateShader` |

### `love.image` — 3/3

| | function |
|---|---|
| :white_check_mark: | `love.image.isCompressed` |
| :white_check_mark: | `love.image.newCompressedData` |
| :white_check_mark: | `love.image.newImageData` |

### `love.joystick` — 2/6

| | function |
|---|---|
| :white_medium_square: | `love.joystick.getGamepadMappingString` |
| :white_check_mark: | `love.joystick.getJoystickCount` |
| :white_check_mark: | `love.joystick.getJoysticks` |
| :white_medium_square: | `love.joystick.loadGamepadMappings` |
| :white_medium_square: | `love.joystick.saveGamepadMappings` |
| :white_medium_square: | `love.joystick.setGamepadMapping` |

### `love.keyboard` — 9/9

| | function |
|---|---|
| :white_check_mark: | `love.keyboard.getKeyFromScancode` |
| :white_check_mark: | `love.keyboard.getScancodeFromKey` |
| :white_check_mark: | `love.keyboard.hasKeyRepeat` |
| :white_check_mark: | `love.keyboard.hasScreenKeyboard` |
| :white_check_mark: | `love.keyboard.hasTextInput` |
| :white_check_mark: | `love.keyboard.isDown` |
| :white_check_mark: | `love.keyboard.isScancodeDown` |
| :white_check_mark: | `love.keyboard.setKeyRepeat` |
| :white_check_mark: | `love.keyboard.setTextInput` |

### `love.math` — 11/16

| | function |
|---|---|
| :white_check_mark: | `love.math.colorFromBytes` |
| :white_check_mark: | `love.math.colorToBytes` |
| :white_check_mark: | `love.math.gammaToLinear` |
| :white_medium_square: | `love.math.getRandomSeed` |
| :white_medium_square: | `love.math.getRandomState` |
| :white_check_mark: | `love.math.isConvex` |
| :white_check_mark: | `love.math.linearToGamma` |
| :white_medium_square: | `love.math.newBezierCurve` |
| :white_check_mark: | `love.math.newRandomGenerator` |
| :white_medium_square: | `love.math.newTransform` |
| :white_check_mark: | `love.math.noise` |
| :white_check_mark: | `love.math.random` |
| :white_check_mark: | `love.math.randomNormal` |
| :white_check_mark: | `love.math.setRandomSeed` |
| :white_medium_square: | `love.math.setRandomState` |
| :white_check_mark: | `love.math.triangulate` |

### `love.mouse` — 8/18

| | function |
|---|---|
| :white_medium_square: | `love.mouse.getCursor` |
| :white_check_mark: | `love.mouse.getPosition` |
| :white_medium_square: | `love.mouse.getRelativeMode` |
| :white_medium_square: | `love.mouse.getSystemCursor` |
| :white_check_mark: | `love.mouse.getX` |
| :white_check_mark: | `love.mouse.getY` |
| :white_medium_square: | `love.mouse.isCursorSupported` |
| :white_check_mark: | `love.mouse.isDown` |
| :white_medium_square: | `love.mouse.isGrabbed` |
| :white_check_mark: | `love.mouse.isVisible` |
| :white_medium_square: | `love.mouse.newCursor` |
| :white_medium_square: | `love.mouse.setCursor` |
| :white_check_mark: | `love.mouse.setGrabbed` |
| :white_medium_square: | `love.mouse.setPosition` |
| :white_check_mark: | `love.mouse.setRelativeMode` |
| :white_check_mark: | `love.mouse.setVisible` |
| :white_medium_square: | `love.mouse.setX` |
| :white_medium_square: | `love.mouse.setY` |

### `love.physics` — 3/22

| | function |
|---|---|
| :white_medium_square: | `love.physics.getDistance` |
| :white_check_mark: | `love.physics.getMeter` |
| :white_medium_square: | `love.physics.newBody` |
| :white_medium_square: | `love.physics.newChainShape` |
| :white_medium_square: | `love.physics.newCircleShape` |
| :white_medium_square: | `love.physics.newDistanceJoint` |
| :white_medium_square: | `love.physics.newEdgeShape` |
| :white_medium_square: | `love.physics.newFixture` |
| :white_medium_square: | `love.physics.newFrictionJoint` |
| :white_medium_square: | `love.physics.newGearJoint` |
| :white_medium_square: | `love.physics.newMotorJoint` |
| :white_medium_square: | `love.physics.newMouseJoint` |
| :white_medium_square: | `love.physics.newPolygonShape` |
| :white_medium_square: | `love.physics.newPrismaticJoint` |
| :white_medium_square: | `love.physics.newPulleyJoint` |
| :white_medium_square: | `love.physics.newRectangleShape` |
| :white_medium_square: | `love.physics.newRevoluteJoint` |
| :white_medium_square: | `love.physics.newRopeJoint` |
| :white_medium_square: | `love.physics.newWeldJoint` |
| :white_medium_square: | `love.physics.newWheelJoint` |
| :white_check_mark: | `love.physics.newWorld` |
| :white_check_mark: | `love.physics.setMeter` |

### `love.sound` — 2/2

| | function |
|---|---|
| :white_check_mark: | `love.sound.newDecoder` |
| :white_check_mark: | `love.sound.newSoundData` |

### `love.system` — 7/8

| | function |
|---|---|
| :white_check_mark: | `love.system.getClipboardText` |
| :white_check_mark: | `love.system.getOS` |
| :white_check_mark: | `love.system.getPowerInfo` |
| :white_check_mark: | `love.system.getProcessorCount` |
| :white_medium_square: | `love.system.hasBackgroundMusic` |
| :white_check_mark: | `love.system.openURL` |
| :white_check_mark: | `love.system.setClipboardText` |
| :white_check_mark: | `love.system.vibrate` |

### `love.thread` — 3/3

| | function |
|---|---|
| :white_check_mark: | `love.thread.getChannel` |
| :white_check_mark: | `love.thread.newChannel` |
| :white_check_mark: | `love.thread.newThread` |

### `love.timer` — 5/6

| | function |
|---|---|
| :white_medium_square: | `love.timer.getAverageDelta` |
| :white_check_mark: | `love.timer.getDelta` |
| :white_check_mark: | `love.timer.getFPS` |
| :white_check_mark: | `love.timer.getTime` |
| :white_check_mark: | `love.timer.sleep` |
| :white_check_mark: | `love.timer.step` |

### `love.touch` — 3/3

| | function |
|---|---|
| :white_check_mark: | `love.touch.getPosition` |
| :white_check_mark: | `love.touch.getPressure` |
| :white_check_mark: | `love.touch.getTouches` |

### `love.video` — 0/1

| | function |
|---|---|
| :white_medium_square: | `love.video.newVideoStream` |

### `love.window` — 20/36

| | function |
|---|---|
| :white_medium_square: | `love.window.close` |
| :white_check_mark: | `love.window.fromPixels` |
| :white_check_mark: | `love.window.getDPIScale` |
| :white_check_mark: | `love.window.getDesktopDimensions` |
| :white_medium_square: | `love.window.getDisplayCount` |
| :white_medium_square: | `love.window.getDisplayName` |
| :white_medium_square: | `love.window.getDisplayOrientation` |
| :white_check_mark: | `love.window.getFullscreen` |
| :white_medium_square: | `love.window.getFullscreenModes` |
| :white_medium_square: | `love.window.getIcon` |
| :white_check_mark: | `love.window.getMode` |
| :white_medium_square: | `love.window.getPosition` |
| :white_medium_square: | `love.window.getSafeArea` |
| :white_medium_square: | `love.window.getTitle` |
| :white_medium_square: | `love.window.getVSync` |
| :white_check_mark: | `love.window.hasFocus` |
| :white_check_mark: | `love.window.hasMouseFocus` |
| :white_medium_square: | `love.window.isDisplaySleepEnabled` |
| :white_medium_square: | `love.window.isMaximized` |
| :white_medium_square: | `love.window.isMinimized` |
| :white_check_mark: | `love.window.isOpen` |
| :white_check_mark: | `love.window.isVisible` |
| :white_check_mark: | `love.window.maximize` |
| :white_check_mark: | `love.window.minimize` |
| :white_check_mark: | `love.window.requestAttention` |
| :white_check_mark: | `love.window.restore` |
| :white_medium_square: | `love.window.setDisplaySleepEnabled` |
| :white_check_mark: | `love.window.setFullscreen` |
| :white_check_mark: | `love.window.setIcon` |
| :white_check_mark: | `love.window.setMode` |
| :white_medium_square: | `love.window.setPosition` |
| :white_check_mark: | `love.window.setTitle` |
| :white_check_mark: | `love.window.setVSync` |
| :white_medium_square: | `love.window.showMessageBox` |
| :white_check_mark: | `love.window.toPixels` |
| :white_check_mark: | `love.window.updateMode` |

## Beyond LOVE

Functions this engine adds. **These do not exist in desktop LOVE**, so a
cart using them is not portable back to it. Excluded from the
percentage above.

- `love.audio.beep`
- `love.debugValue`
- `love.filesystem.exists`
- `love.filesystem.load_save`
- `love.graphics.__resetStack`
- `love.log`
- `love.mark`
- `love.mouse.isRelativeMode`
- `love.net.broadcast`
- `love.net.close`
- `love.net.count`
- `love.net.isOpen`
- `love.net.name`
- `love.net.open`
- `love.net.peers`
- `love.net.send`
- `love.net.state`
- `love.net.transport`
- `love.pad.axis`
- `love.pad.getVibration`
- `love.pad.hasVibration`
- `love.pad.isDown`
- `love.pad.setVibration`
- `love.pad.stopVibration`
- `love.pad.wasPressed`
- `love.pad.wasReleased`
- `love.physics.World`
- `love.physics.stats`
- `love.window.focus`
- `love.window.getDimensions`
- `love.window.getHeight`
- `love.window.getWidth`
