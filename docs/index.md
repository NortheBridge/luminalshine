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

::::{grid} 1 1 2 2
:gutter: 3
:padding: 0

:::{grid-item-card} 🚀 Quick Start
:link: quick_start
:link-type: doc

The 10-minute happy path: install, pair a client, start streaming.
:::

:::{grid-item-card} 📖 Introduction
:link: introduction
:link-type: doc

What LuminalShine is, why it exists, and how it relates to
Sunshine + Vibeshine.
:::

:::{grid-item-card} 💻 System Requirements
:link: system_requirements
:link-type: doc

Host PC requirements, supported encoders, and what clients work.
:::

:::{grid-item-card} 🏛️ Architecture
:link: architecture
:link-type: doc

What's running on your PC: capture, encode, transport, input replay.
:::

::::

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

getting_started
building
configuration
```

```{toctree}
:caption: Guides
:maxdepth: 2

guides
app_examples
performance_tuning
troubleshooting
gamestream_migration
```

```{toctree}
:caption: Reference
:maxdepth: 2

api
api/cpp
third_party_packages
awesome_sunshine
```

```{toctree}
:caption: Project
:maxdepth: 1

contributing
changelog
legal
```
