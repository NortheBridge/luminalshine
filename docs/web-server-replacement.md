# Replacing Simple-Web-Server — evaluation and recommendation

Status: recommendation only (2026-07-27). Not scheduled for beta.6 — the beta.6
fix hardens the vendored Simple-Web-Server in place (NortheBridge fork). This
document is the follow-up plan for retiring it.

## Why replace it

The 2026-07-27 crash analysis (`POSTMORTEM-2026-07-27.md`; dump
`luminalshine.exe.52192.dmp`) traced the nightly host death to a
session-lifetime race inside Simple-Web-Server's HTTPS server: two independent
write-submission paths per response (`send()` queue vs `send_on_delete`), an
unsynchronized keep-alive streambuf hand-off, and no strand serialization of
per-connection state. The library is a ~900-line header maintained sporadically
(our submodule is a LizardByte infrastructure fork); its design predates
asio strands-by-default, and every LuminalShine feature that holds a response
across threads (SSE ICE streaming, task_pool handlers) fights the model.

## What confighttp/nvhttp actually need

- HTTPS (OpenSSL; our own certs), HTTP/1.1 keep-alive; TLS client-cert
  verification on the nvhttp GameStream plane.
- REST routing with regex/param paths (~120 routes), large static/SPA serving
  with range requests, cookie/session + token auth middleware.
- Server-Sent Events (ICE candidate stream) and long-held responses written
  from non-server threads — the thing that must be first-class.
- WebSocket: not currently used (WebRTC signaling is REST+SSE), but keep the
  door open.
- Windows/clang(MinGW, `--fms-extensions`)/CMake build; no MSVC-only code;
  header-only or trivially-linkable strongly preferred (our CI builds cold).
- Low idle overhead — the host is a always-on SYSTEM service; a thread-per-core
  event loop or single-threaded io model both fine.

## Candidates

| Library | Model | TLS | Fit notes |
|---|---|---|---|
| **Boost.Beast** | asio, low-level HTTP/WS primitives | via asio::ssl | Already ship Boost + asio + OpenSSL — zero new deps, no new build risk on clang/MinGW. No router/middleware: we build a thin server layer (routing table, session mgmt) ourselves — which we already effectively own today, scattered through confighttp. Strand-per-connection is idiomatic. WebSocket built in. Most work, most control, least dependency risk. |
| **Drogon** | own event loop (trantor), full framework | OpenSSL | Fastest full-featured option (routing, filters/middleware, WS, SSE-able chunked responses, static files). C++17, CMake, actively maintained; MinGW builds are supported but less exercised than MSVC/Linux — needs a spike. Brings its own event loop next to our asio usage (two reactor stacks in-process). |
| **Crow** | header-only, asio-based | via asio::ssl | Familiar Flask-like API, header-only, middleware, WS. Maintained community fork (CrowCpp). asio-based so it composes with our stack. Weaker SSE story (manual chunked writes); route performance fine at our scale. |
| **cpp-httplib** | header-only, thread-per-connection blocking | OpenSSL | Simplest possible integration; SSE supported via content providers. But thread-per-connection + blocking model is a regression for an always-on service with long-held SSE streams (thread per open stream), and its Windows TLS cert-verification paths need care. Good for tools, not for the host. |
| **uWebSockets** | own event loop (uSockets), WS-first | BoringSSL/OpenSSL | Exceptional performance, but C-ish API, BoringSSL preference, and MinGW support is rough; HTTP feature set (routing/static/middleware) is thin. Overkill for a config UI, wrong shape for our TLS client-cert needs. |
| Pistache / Oat++ / RESTinio | various | various | Pistache is Linux-only (out). Oat++ is full-featured but brings a large opinionated object model and its own async runtime. RESTinio is asio-based and decent but smaller community than Crow/Drogon; SSE workable via chunked writes. None beat the top three for us. |

## Recommendation

1. **First choice: Boost.Beast** with a small in-repo server layer
   (`src/httpd/`): connection class = one strand + one SSL stream + explicit
   state machine; router lifted from confighttp's existing regex table; SSE as
   a first-class "streaming response" object that marshals writes onto the
   connection strand (the exact bug class we just fixed becomes structurally
   impossible). Zero new dependencies, full control of session lifetime, and
   both confighttp and nvhttp (client-cert plane) can migrate onto the same
   layer incrementally — route by route — while Simple-Web-Server keeps
   serving the unmigrated routes during the transition.
2. **Second choice: Crow (CrowCpp)** if we prefer not to own a server layer:
   header-only, asio-composable, small diff for the 80% of routes that are
   plain JSON REST. SSE and client-cert verification need custom work anyway,
   which narrows its advantage over Beast.
3. Drogon is the best "batteries included" framework but the second event loop
   and MinGW risk make it a spike-first option, not a default.

Suggested sequencing (post-Milestone 1): spike Beast connection layer behind a
compile flag, port `/api/health/*` + static serving first, then the auth
middleware, then SSE, then nvhttp last (client-cert plane needs the most care).
Delete the Simple-Web-Server submodule at the end.
