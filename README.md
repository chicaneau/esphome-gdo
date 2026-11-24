# esphome-gdo [![Made for ESPHome](https://img.shields.io/badge/Made_for-ESPHome-black?logo=esphome)](https://esphome.io)

This [ESPHome](https://esphome.io) external component allows control of a Garage Door Opener with a relay and one or two reed sensors. Supports:

- open/close/stop control
- most importantly position reporting and control, distinguishing it from other similar projects
- obstruction sensor

See the included `example-gdo.yaml` for my personal setup with just one reed sensor at the fully-open position.

## Configuration

Add the component to your config:

```yaml
external_components:
  - source: github://chicaneau/esphome-gdo@main
```

### `cover` platform `gdo`

| Option                | Type     | Description                                                        |
|-----------------------|----------|--------------------------------------------------------------------|
| `open_duration`       | Required | Time the door takes to travel from fully closed to fully open.     |
| `close_duration`      | Required | Time the door takes to travel from fully open to fully closed.     |
| `single_press_action` | Required | Automation that pulses the relay once.                             |
| `double_press_action` | Required | Automation that pulses the relay twice.                            |
| `triple_press_action` | Required | Automation that pulses the relay thrice.                            |
| `open_endstop`        | Optional | ID of a binary sensor that reads on when the door is fully open.   |
| `close_endstop`       | Optional | ID of a binary sensor that reads on when the door is fully closed. |
| `start_delay`         | Optional | Grace after a start press before an auto stop press may be sent. Default `1s`. |
| `press_interval`      | Optional | Delay between relay clicks in the double/triple press automations. Must match the YAML; default `160ms`. |

At least one of `open_endstop` / `close_endstop` is required. Without one the
position estimate can never be corrected, so it drifts.

The durations are used two ways: to interpolate the position while the door
moves, and as the basis for the endstop timeout. The timeout allows 25% plus 2s
over the configured duration before it gives up and reports an unknown position,
so the durations do not have to be exact to the millisecond.

Position is held just short of 100% / 0% until the corresponding endstop
actually confirms it, so the cover does not read "fully open" while the door is
still moving.

When asked for a partial position, the firmware sends the start press and then
a stop press once the time estimate reaches the target. But the opener needs a
moment after the last click to physically start moving, so on short moves the
stop press would otherwise land while the door is still standing still — and
an opener that sees an idle door treats that press as a new start press rather
than a stop, running the door all the way to its limit while the cover already
reports it stopped. The firmware therefore never sends the auto stop press
earlier than the click-sequence duration (`press_interval` × clicks) plus
`start_delay`, by which point the door is definitely moving and a single press
reliably stops it. The trade-off is that very short partial moves overshoot the
target by about that grace window (roughly 8% of a full travel with default
durations); lower `start_delay` if your opener reacts faster.

### `binary_sensor` platform `gdo`

Reports the state of the safety obstruction sensor.

| Option           | Type     | Description                                                                                                       |
|------------------|----------|-------------------------------------------------------------------------------------------------------------------|
| `input_obst_pin` | Required | Pin wired to the obstruction sensor circuit. Must be a pin on the ESP itself, since it is read with an interrupt. |

Defaults to `device_class: problem` and `entity_category: diagnostic`.

## Troubleshooting

**A partial move (open/close to a %) runs to the end limit and the relay never
clicks a second time.** Reed switches chatter for a few hundred milliseconds
when the door pulls away from the endstop magnet at the start of a travel. A
spurious "endstop reached" blip that reaches the cover logic in time aborts the
move, and the release right after it is then misread as an external full
travel. Fix: give the endstop binary sensors a real debounce, e.g.
`filters: - delayed_on: 300ms` (the example config defaults to 300ms; 20ms is
not enough for most reeds). The firmware additionally ignores "reached"
events that contradict the current direction of travel, so short chatter is
harmless even without the filter, but very long blips (>300ms) still need it.
Confirm with the device log: `Closed endstop reported reached while opening -
chatter, ignoring.` shows the guard active, and a real partial move ends with
`Reached target NN% after N.Ns. Sending stop press.`

## Hardware requirements

- A Garage Door Opener that you open/close with a single button. The behavior of the button is expected to be:
    - If door is closed a single press opens it.
    - If door is fully open a single press closes it.
    - If door is opening a single press stops it.
    - If door is closing a single press stops it.
    - if door is partially open, a single press will open or close it in the opposite direction to what it was when it was last opening or closing
- ESP board [compatible](https://esphome.io/#devices) with ESPHome.
- Relay to either press the physical button of the wall control panel (for Chamberlain Security + 2.0) or short the controls on the garage door opener itself (for Chamberlain Security + 1.0 or Genie etc.).
- One or two reed sensors to detect the fully-open and/or fully-closed states. If using a single reed sensor, it can be placed in either fully-open or fully-closed positions.
- Optional two 10kΩ resistors to detect the obstruction sensor.

## Credits

- Adopted from [Endstop Cover](https://esphome.io/components/cover/endstop) and [Time Based Cover](https://esphome.io/components/cover/time_based).
- The circuit for the obstruction sensor is from [rat-ratgdo](https://github.com/Kaldek/rat-ratgdo).
- The code for the obstruction sensor is from [esphome-ratgdo](https://github.com/ratgdo/esphome-ratgdo).

## Dev notes

```sh
python3 -m venv .venv
source .venv/bin/activate
# for Windows CMD:
# .venv\Scripts\activate.bat
# for Windows PowerShell:
# .venv\Scripts\Activate.ps1

pip install esphome pre-commit

pre-commit install

pre-commit run --all-files

# Compile with local code instead of pulling from Github
esphome -s external_components_source components compile example-gdo.yaml

# Deploy local code
esphome -s external_components_source components run example-gdo.yaml

# Validate the same configs CI does
esphome config tests/test-*.yaml
```
