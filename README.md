# freeswitch-modules

Custom FreeSWITCH image with `mod_audio_fork` for bidirectional WebSocket audio streaming. Originally based on code from [drachtio-freeswitch-modules](https://github.com/dochong/drachtio-freeswitch-modules), heavily modified and trimmed to a single module.

## mod_audio_fork

Attaches a media bug to a FreeSWITCH channel and streams bidirectional audio over a WebSocket connection.

**Upstream behavior** (send-only): Captures call audio in L16 format and streams it to a remote WebSocket server.

**Our additions** (bidirectional): Binary audio frames received on the WebSocket are buffered in a ring buffer and injected back into the call via `SMBF_WRITE_REPLACE`. This enables real-time TTS playback without file I/O — the voice gateway streams synthesized audio back through the same WebSocket that carries STT audio.

### API

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> [metadata]
uuid_audio_fork <uuid> stop
```

- `mix-type`: `mono` (caller only), `mixed` (both parties, single channel), `stereo` (two channels)
- `metadata`: optional JSON sent as initial text frame after connection

## Building

The Dockerfile builds FreeSWITCH 1.10 from source (with only the modules needed for voice gateway) and compiles `mod_audio_fork` on top.

```bash
docker build -t drellia/freeswitch-mrf .
```

The first build takes a while (~15-20 min) since it compiles FreeSWITCH from source. Subsequent builds are fast due to Docker layer caching — only the `mod_audio_fork` compilation layer is re-run when module code changes.

## Project Structure

```
├── Dockerfile                      # Multi-stage build (FreeSWITCH + mod_audio_fork)
├── modules/
│   └── mod_audio_fork/
│       ├── mod_audio_fork.c        # FreeSWITCH module entry point
│       ├── mod_audio_fork.h        # Shared types (cap_cb, ring buffer)
│       ├── lws_glue.cpp            # libwebsockets client (send + receive)
│       ├── lws_glue.h              # C API for session init/cleanup/frame
│       └── Makefile.am             # Autotools build (unused in Docker build)
└── README.md
```

## Included FreeSWITCH Modules

Only the modules required by the voice gateway are compiled:

| Module | Purpose |
|--------|---------|
| mod_sofia | SIP endpoint (calls) |
| mod_event_socket | ESL interface (call control) |
| mod_commands | API commands |
| mod_dptools | Dialplan tools (answer, bridge, etc.) |
| mod_dialplan_xml | XML dialplan |
| mod_audio_fork | **Custom** — bidirectional WebSocket audio |
| mod_sndfile | Audio file format support |
| mod_native_file | Native file playback |
| mod_tone_stream | Tone generation |
| mod_loopback | Loopback endpoint |
| mod_console | Console logging |
| mod_logfile | File logging |

## Usage with voice-gateway

The voice-gateway's `docker-compose.dev.yml` builds this image and mounts FreeSWITCH config files. The gateway triggers `uuid_audio_fork` via ESL after placing a call, establishing a bidirectional audio WebSocket for the STT/TTS pipeline.
