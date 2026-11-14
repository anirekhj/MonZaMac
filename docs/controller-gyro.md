# Controller gyro camera

At the top of **Controls**, check **Enable gyro camera** to turn camera motion on, or uncheck it to turn it off.
Disabling it preserves the **Gyro activation** choice, sensitivity and inversions, including across app restarts.
Under **Controller gyro options**, choose **Gyro activation → Always** for continuous motion without a trigger.
Gyro-derived right-stick input remains available in the game's own menus; the game determines how each menu responds
to it. Existing files without gyro keys start disabled, with Always ready as the activation choice.

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
calibrating, ready, or sensor error. Check **Enable gyro camera** before using the calibration button. Controller detection may not
update until the game starts if the input service is not initialized yet.

A controller and connection must expose a gyro through SDL; the presence of controller buttons alone does not
establish sensor support. Sensor errors or unsupported connections leave the normal stick controls available.
Physical direction, sensitivity and wireless behavior still need a real-controller playtest; synthetic tests do
not establish how gyro feels on a DualSense.

The input follows [SDL's sensor coordinate convention](https://wiki.libsdl.org/SDL3/SDL_SensorType): angular velocity
in radians per second, with local X for pitch, Y for yaw, and Z for roll. When accelerometer data is usable, the
camera mapping estimates gravity for yaw and projects the pitch axis perpendicular to it; it falls back to local
axes otherwise. The optional camera mapping and the game's native motion input are separate consumers of the sensors.

## Native game motion

By default, native motion is automatic: the game starts and stops its SixAxis sensor. Check **Always provide controller
motion**, beside **Enable gyro camera** in Controls, to keep real physical acceleration, rotation and orientation
available even if the game leaves its sensor stopped. Uncheck it to return to automatic activation. This option
starts disabled for existing settings files. It is independent of **Enable gyro camera**, camera sensitivity,
inversion and recalibration. Ordinary MonZaMac Controls overlays do not reset it. Loss of window focus, hidden or
minimized windows, preparation/launcher pages, scripted input, disconnected devices and stale reports suppress native
readings. Sensor channels are enabled only while the camera, a guest sensor request or this explicit Always option needs them.

Direct SDK sensor starts and HID service commands 66/67 share the same activation state. Starting an already
active handle is idempotent, and stopping one handle keeps sensing enabled while another handle needs it.
Enumerating handles or reading states does not change those activation flags; the Always option overrides native sample availability.
The `motion` diagnostic identifies the activation
source as `hle` or `ipc` so a physical-controller run can confirm which route the game uses.

The virtual controller matches **Video → Game mode**: Docked uses player 1 / FullKey; Handheld uses the handheld
controller ID and style. A dock/undock also signals controller-style watchers. Restart the game if its response to
a live mode change is incomplete; selecting the mode in the launcher before Continue avoids that dependency.

For an [Inkay evolution test](https://www.gamesradar.com/games/pokemon/pokemon-legends-z-a-inkay-evolve-malamar/),
use **Handheld** mode with an Inkay at level 30 or higher in the party, and check **Always provide controller motion**.
Hold the controller upside down while
opening Inkay's party-menu actions and select **Evolve**. Camera gyro can be disabled for this. The native motion
and handheld paths have synthetic regressions; the actual evolution still needs a physical-controller playtest.

The adapter follows the public [libnx state layout and sensor handles](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h),
converting SDL units and axes into native acceleration in G and rotation in revolutions.
[This native motion implementation reference](https://github.com/seedds/nxnative/blob/main/dm/host/motion_fusion.h)
documents the coordinate and direction-matrix conventions. The host integrates angular velocity and corrects tilt
with gravity; it does not reproduce Nintendo's proprietary fusion filter. Fusion/drift preferences round-trip,
but those proprietary gain and drift policies are not emulated. GetStates returns one actual latest sample;
polling does not fabricate extra samples. The default-off `motion` developer-log subject reports handle requests,
sensor availability/ages and bounded orientation diagnostics. Read diagnostics identify the API and guest caller;
handle enumeration and direct Start/Stop requests also report their guest caller.

Example settings for continuous gyro in `larecomp.ini`:

```ini
gyro_camera = always
gyro_camera_mode = always
gyro_speed = 1
invert_gyro_x = 0
invert_gyro_y = 0
```

`gyro_camera` remains the effective setting: `off`, `hold`, or `always`; invalid values become `off`.
`native_motion_always = 1` enables the explicit native-motion override; `0` is automatic and remains the default
when the key is absent or malformed. It does not enable gyro camera mapping.
`gyro_camera_mode` remembers `hold` or `always` while the camera is off. Older files with an active mode keep it;
an active `gyro_camera` value takes precedence over a conflicting remembered mode. Finite sensitivity values are
clamped to 0.1–5.0, and malformed or nonfinite values use 1.0. Changes apply while playing and are saved with the other
Controls options. Close the app before editing its settings file manually.

CPU-only math, SDL event integration and settings checks use authored fixtures and temporary files;
they need no controller or game data. The SDL integration test stubs device access and never initializes SDL:

```sh
python3 tools/test_gyro_camera.py
python3 tools/test_controller_motion.py
python3 tools/test_sixaxis.py
python3 tools/test_gyro_input.py
python3 tools/test_gyro_settings.py
```
