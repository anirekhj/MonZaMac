# Controller gyro camera

For continuous gyro, choose **Controls → Controller gyro → Gyro camera → Always**. Turn the controller to move
the camera without holding a trigger. Gyro-derived right-stick input remains available in the game's own menus;
the game determines how each menu responds to it. The default is **Off**, including existing settings files without
gyro keys; choose Always to enable continuous motion.

**Hold L2 / ZL** is an optional alternative. It uses the physical left trigger: L2 on PlayStation/DualSense, LT on
Xbox-style layouts, or ZL on Nintendo layouts. Releasing it stops gyro camera input. Button remapping does not
change this physical trigger, and it keeps its ordinary game action.

Gyro adds camera input alongside the right stick. **Gyro sensitivity** adjusts it from 10% to 500%; 100% is the
default. **Invert gyro horizontal** and **Invert gyro vertical** work independently of the mouse inversion settings.
Gyro camera input pauses only in MonZaMac's overlay/name-entry UI or launcher, and when the game window is hidden,
minimized or lacks focus. It does not use the game's own menus as an activation gate.

For calibration, keep MonZaMac's Controls menu open and rest the controller still for about half a second, until
**Gyro status** shows **Ready**. In Hold mode, also release L2 / LT / ZL. A quiet inactive controller learns its
resting bias automatically. **Calibrate gyro** starts that process over if the camera drifts. Reconnecting clears
calibration. Entering or leaving MonZaMac's overlay clears pending motion while preserving the learned bias;
Hold mode also does this when its trigger is released. The status row reports disconnected, unavailable, off,
calibrating, ready, or sensor error. Enable gyro before using the calibration button. Controller detection may not
update until the game starts if the input service is not initialized yet.

A controller and connection must expose a gyro through SDL; the presence of controller buttons alone does not
establish sensor support. Sensor errors or unsupported connections leave the normal stick controls available.
Physical direction, sensitivity and wireless behavior still need a real-controller playtest; synthetic tests do
not establish how gyro feels on a DualSense.

The input follows [SDL's sensor coordinate convention](https://wiki.libsdl.org/SDL3/SDL_SensorType): angular velocity
in radians per second, with local X for pitch, Y for yaw, and Z for roll. When accelerometer data is usable, the
camera mapping estimates gravity for yaw and projects the pitch axis perpendicular to it; it falls back to local
axes otherwise. It does not send motion data to the game's native gyro interface.

Example settings for continuous gyro in `larecomp.ini`:

```ini
gyro_camera = always
gyro_speed = 1
invert_gyro_x = 0
invert_gyro_y = 0
```

`gyro_camera` accepts `off`, `hold`, or `always`; invalid values become `off`. Finite sensitivity values are clamped
to 0.1–5.0, and malformed or nonfinite values use 1.0. Changes apply while playing and are saved with the other
Controls options. Close the app before editing its settings file manually.

CPU-only math, SDL event integration and settings checks use authored fixtures and temporary files;
they need no controller or game data. The SDL integration test stubs device access and never initializes SDL:

```sh
python3 tools/test_gyro_camera.py
python3 tools/test_gyro_input.py
python3 tools/test_gyro_settings.py
```
