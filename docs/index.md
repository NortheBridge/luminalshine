---
hide-toc: false
---

# LuminalShine

**Game streaming, rebuilt for modern Windows.**

LuminalShine is a self-hosted, low-latency game streaming host for
Windows 11 — a hardened, NortheBridge-maintained descendant of
[Sunshine](https://github.com/LizardByte/Sunshine), focused
exclusively on getting the most out of current Windows builds
(including the Insider Preview Canary and Dev channels). Pair it
with any [Moonlight](https://moonlight-stream.org/) client, or
stream straight to a browser via the built-in WebRTC client at
`/webrtc`.

**New here?** Follow the [Quick Start](quick_start.md) to install LuminalShine,
pair your client, and start your first stream.

## Start streaming

| I want to… | Start here |
| --- | --- |
| Get my first stream running | [Quick Start](quick_start.md) — install, pair, and play. |
| Check my host and client | [System Requirements](system_requirements.md) — hardware, encoders, and supported clients. |
| Configure my stream | [Configuration](configuration.md) — video, audio, input, and network settings. |
| Tune performance | [Performance Tuning](performance_tuning.md) — encoder and GPU guidance. |
| Fix a problem | [Troubleshooting](troubleshooting.md) — help with setup and streaming. |

## Understand and build LuminalShine

| Guide | What it covers |
| --- | --- |
| [Introduction](introduction.md) | The project and its relationship to Sunshine and Vibeshine. |
| [Architecture](architecture.md) | Capture, encoding, transport, and input. |
| [Building](building.md) | Toolchains and building from source. |
| [API](api.md) | Integrating with the host. |
| [Changelog](changelog.md) | Changes in each release. |

```{toctree}
:caption: Foundations
:maxdepth: 2
:hidden:

introduction
system_requirements
quick_start
architecture
concepts
```

```{toctree}
:caption: Install and Configure
:maxdepth: 2
:hidden:

getting_started
building
configuration
```

```{toctree}
:caption: Guides
:maxdepth: 2
:hidden:

guides
app_examples
performance_tuning
troubleshooting
gamestream_migration
```

```{toctree}
:caption: Reference
:maxdepth: 2
:hidden:

api
api/cpp
third_party_packages
awesome_sunshine
```

```{toctree}
:caption: Project
:maxdepth: 1
:hidden:

contributing
changelog
legal
```
