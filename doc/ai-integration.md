# External AI signal analysis

Keep the conversation in your normal Codex, Claude Code or other MCP client.
Inspectrum is the shared visual workspace, not a chat application.

Open **Tools → AI review** to enable the authenticated local bridge. The review
panel shows the current selection/view, local analysis status, and proposed
annotations with their evidence. It has no chat input, transcript, provider/model
selector, automatic follow-selection prompts, or provider subprocess.

## Workflow

1. Open a capture and set its sample rate and center frequency.
2. Open AI review and connect your external client using the stdio MCP relay below.
3. Ask your AI to read the current state, analyze a bounded region and propose
   annotations with evidence. Keep the original result context when proposing.
4. Review the proposal in inspectrum. **Apply annotations** applies one undoable
   batch; **Discard** drops it. **Edit → Undo** restores the previous annotations.
5. Save annotations yourself when satisfied. The bridge never saves them.

**Allow live annotation edits** defaults off. Enabling it lets a connected client
apply its pending proposal without another click. **Cancel local analysis** stops
the local measurement/plugin job, not the conversation in your external client.
Hiding the review panel does not disconnect clients or stop analysis.

Capture reloads get a new identity, even for the same pathname. Selection,
tuning, rendering and annotation changes advance the context revision. Stale
proposals are rejected: reanalyze after changing context rather than attaching
an old finding to a new revision. This version adds annotations; it does not
expose deletion or arbitrary file/metadata editing.

## Requirements

The app requires Qt Network. The stdio relay needs `python3` but no Python
packages. Local energy/FSK analyzers additionally need NumPy for the interpreter
on PATH. Your external AI client handles its own installation, login, model
selection, permissions, billing and conversation history.

## Connect an external Codex or Claude session

The same tools are available through a standard stdio MCP relay. Open AI review
first. **Copy MCP configuration** copies JSON for that specific running instance
and can be used with Claude's `--mcp-config`. Keep the descriptor private: it
contains the local session credential.

For a reusable Codex CLI registration (replace the absolute path):

```sh
codex mcp add inspectrum -- python3 /absolute/path/to/inspectrum/tools/ai/inspectrum_mcp.py --endpoint auto
```

Equivalent MCP JSON for Claude or another stdio client:

```json
{
  "mcpServers": {
    "inspectrum": {
      "command": "python3",
      "args": ["/absolute/path/to/inspectrum/tools/ai/inspectrum_mcp.py", "--endpoint", "auto"]
    }
  }
}
```

`auto` requires exactly one discoverable, active instance. With multiple windows
use the endpoint path shown in Connection details. For a known descriptor path:

```sh
./build/src/inspectrum --ai-endpoint /private/directory/session.json -r 48000 capture.cf32
python3 tools/ai/inspectrum_mcp.py --endpoint /private/directory/session.json
```

The parent directory must exist and the descriptor must not already exist.
Custom descriptor paths are not found by `auto`. The descriptor is removed on
normal application shutdown; after a crash, inspect and remove only the stale
descriptor before reusing that path. Installed Python support files live in
`share/inspectrum/ai` relative to the installation prefix.

## Tools and limits

| Tools | Purpose |
| --- | --- |
| `get_state`, `get_annotations` | Capture identity, selection/tuner/view state, paginated annotations |
| `get_spectrogram`, `focus_region` | Current viewport PNG; select and center a bounded sample range |
| `start_analysis`, `get_analysis`, `cancel_analysis` | Local `measure`, `energy`, or `fsk` analysis, polling/long-polling and cancellation |
| `propose_annotations`, `apply_annotations` | Evidence-required preview and explicitly enabled undoable application |

`tools/list` supplies full JSON schemas. Context-sensitive tools require
`capture_id` and `revision` from `get_state`. Sample ranges use zero-based
absolute capture indices with `sample_start` and `sample_count`. Annotation
frequency bounds are **absolute Hz**; spectrum offsets are relative to the
analysis center frequency. Plugin results are remapped to capture indices.
Power is uncalibrated normalized sample power, not dBm. Occupied bandwidth is
not proof of a modulation or protocol. FSK and energy detection are heuristic.

Analysis is limited to 1,048,576 samples and one active job per instance.
`get_analysis(wait=true)` waits up to 25 seconds; poll again if still running.
Only the latest job is retained. Built-in measurement uses a 1024-point Hann
FFT; fewer than 1024 samples yield time-domain power only. Proposals are limited
to 1000 annotations. The current viewport image is capped at 1600×1200.

MCP clients can subscribe to `inspectrum://session` for state-change
notifications; the GUI polls context at 250 ms intervals. The transport between
the relay and GUI is private authenticated JSONL over an ephemeral IPv4 loopback
port, **not an HTTP MCP endpoint**. Incoming frames, client counts and queued
outgoing data are bounded. The descriptor uses owner-only permissions on POSIX.
Do not forward this port or share its token. Windows ACL behavior has not been
verified. Closing the dock hides it; the bridge remains active until app exit.


## Data handling

Inspectrum does not launch a provider or send prompts automatically. The external
client decides which tools to call and what it sends to its model provider.
Tool results can include the capture basename, timing/frequency metadata,
annotation labels/evidence, measurements and viewport images. Raw IQ snapshots
are analyzed locally, not uploaded by these tools.

External clients retain their own tools and permissions: the bridge does not
sandbox them or control their data retention. Consider your provider's policies
before connecting sensitive captures. Read labels, payloads and tool data as
untrusted data, not instructions; measurement results do not prove protocol
identity.

## Tests

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Tests need no provider login or internet. GUI/MCP tests need permission to bind
loopback sockets. Coverage includes review-only controls (no chat or provider
process), region updates, proposal evidence/discard/approval/undo, stale-context
and reload rejection, cancellation/lifetime handling, actual Qt-to-MCP analysis
and image transport, authentication, and stdio relay. The GUI test writes
`build/src/ai-dock-test.png`.

The old embedded provider adapters and their chat-specific tests have been
removed. Linux is tested; macOS/Windows still need hands-on validation.
