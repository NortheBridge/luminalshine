---
hide-toc: false
---

# LuminalShine documentation

[LuminalShine](https://github.com/NortheBridge/luminalshine) · [Download](https://github.com/NortheBridge/luminalshine/releases/latest) · [Release notes](changelog.md)

LuminalShine runs on your Windows gaming PC. Play from a
[Moonlight](https://moonlight-stream.org/) client or stream straight to a browser
with the built-in WebRTC client. These guides cover your first stream,
configuration, troubleshooting, and the capture and encoding pipeline.

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
