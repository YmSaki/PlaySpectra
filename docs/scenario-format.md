# JSON scenario format

A scenario is a JSON object with a name and an ordered steps array:

~~~json
{
  "name": "assert_demo",
  "steps": [
    {"cmd": "hello", "role": "writer"},
    {"cmd": "move_head", "to": {"position": [0.0, 1.6, -1.5]}, "duration_ms": 300},
    {"cmd": "assert", "get": ["hmd", "head", "position", 2],
     "op": "near", "value": -1.5, "tol": 0.02}
  ]
}
~~~

The complete checked-in example is tools/scenarios/assert_demo.json. It checks the initial head position, moves the head, turns it, sets a right trigger, and resets it. A scenario containing assertions is self-checking: the runner exits non-zero when an assertion fails.

## Operation steps

| Command | Arguments | Meaning |
| --- | --- | --- |
| hello | role: writer or observer | Open a control-channel session and acquire the requested role. |
| move_head | to: position and optional orientation; duration_ms | Interpolate the HMD pose. Coordinates are in STAGE space. |
| look | yaw_deg; duration_ms | Turn the head around world up (+Y). |
| move_controller | hand; to: position and optional orientation; duration_ms | Interpolate a controller grip and aim pose. |
| walk_forward | speed; duration_ms; hand | Hold the hand thumbstick forward, then release it. |
| strafe | speed; duration_ms; hand | Hold the hand thumbstick sideways, then release it. |
| trigger | hand; value; duration_ms | Hold a trigger value, then release it. |
| set_input | hand; path; value; optional duration_ms | Set a declared controller input path. |
| press | hand; button; ms | Press and release a button. Right-hand buttons are a/b; left-hand buttons are x/y. |
| wait | ms | Pause within the scenario. |
| reset | none | Restore the builder-initial virtual-device state. |

## Observe and assertion steps

| Command | Arguments | Meaning |
| --- | --- | --- |
| assert | get path; op; value; tol; name; optional timeout_ms and poll_ms | Compare a field in the live get_state response. Operators are near, eq, ne, gt, lt, true, and false. |
| wait_for | get path; op; value; tol; timeout_ms; optional poll_ms and name | Poll get_state until the condition is met or the timeout expires. |
| capture | name; eye | Capture a reference image through the layer and retain its hash. |
| assert_capture | ref; op changed or stable; eye; name; optional timeout_ms and poll_ms | Compare a new capture with a stored reference. |

An assertion path is a JSON array walking the state response, for example ["hmd", "head", "position", 2]. The default eye is left. The MCP wait_for tool accepts the same path serialized as JSON text.

## Running scenarios

~~~bash
python3 tools/playspectra_server.py tools/scenarios/walk_and_look.json
python3 tools/playspectra_server.py tools/scenarios/assert_demo.json
python3 tools/playspectra_server.py tools/scenarios/capture_assert_demo.json --capture-port 52700
~~~

The visual scenario needs the layer loaded in the application. The server's adapter port defaults to 52702 and can be changed with --port.

## Recording format

playspectra_record.py writes a JSON object shaped like this:

~~~json
{
  "name": "recording",
  "rate_hz": 60.0,
  "frames": [
    {"t_ms": 0.0, "state": {"hmd": {}, "left": {}, "right": {}}}
  ]
}
~~~

The real state contains the complete HMD/controller state returned by get_state. A Recorder connects as an observer; a Replayer connects as the writer and emits fresh monotonic sequence values.

~~~bash
python3 tools/playspectra_record.py record out.json --duration-ms 3000 --rate 60
python3 tools/playspectra_record.py replay out.json
~~~

## Coordinate conventions

- Positions are metres in right-handed STAGE space: +X right, +Y up, -Z forward.
- Quaternions use [x, y, z, w].
- Controller pose commands address the grip and aim pose together.

