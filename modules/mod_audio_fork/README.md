# mod_audio_fork

A FreeSWITCH module that attaches a media bug to a channel and streams bidirectional audio over a WebSocket connection in real-time.

## API

### Commands

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> <sampling> [metadata]
```
Attaches media bug and starts streaming audio to the WebSocket server. Also accepts binary audio frames back from the server and injects them into the call (write-replace).

- `uuid` - unique identifier of FreeSWITCH channel
- `wss-url` - WebSocket URL to connect to
- `mix-type`:
  - `mono` - single channel containing caller's audio
  - `mixed` - single channel containing both caller and callee audio
  - `stereo` - two channels with caller audio in one and callee audio in the other
- `sampling` - `8k` or `16k`
- `metadata` - optional JSON metadata sent as initial text frame after connection

```
uuid_audio_fork <uuid> stop
```
Closes WebSocket connection and detaches media bug.

### Bidirectional Audio

The module uses `SMBF_WRITE_REPLACE` to inject audio received from the WebSocket back into the call. Incoming binary WebSocket frames are written to a ring buffer, which is drained at the call's real-time frame rate (typically 20ms frames). This enables low-latency TTS playback without file I/O.

### Events

None.
