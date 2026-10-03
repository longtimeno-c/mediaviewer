# MediaViewer design reference

How MediaViewer works today, one subsystem per doc. These replace the original `plan/` spec, which
was retired on 2026-10-03 once it had been built; its full text is in git history
(`git show 4da0f9c:plan/<file>`). Code comments cite these docs as `docs/design/NN §section`.

Build, run and test recipes are in [../DEVELOPMENT.md](../DEVELOPMENT.md). Forward plans that are not
built yet live in [../plans/](../plans/).

| Doc | Covers |
|---|---|
| [01-decisions.md](01-decisions.md) | The stack and the settled decisions D1–D9 |
| [02-architecture.md](02-architecture.md) | Module layout, dependency rules, threads, frame loop, cancellation, memory budgets |
| [03-rendering.md](03-rendering.md) | Swapchain, DirectComposition hosting, frame pacing, colour, resampling |
| [04-image-pipeline.md](04-image-pipeline.md) | Still formats, decode, colour, progressive display, tiles, prefetch, thumbnails |
| [05-video-pipeline.md](05-video-pipeline.md) | Clip probe, demux, hardware decode, A/V clock, seek |
| [06-metadata.md](06-metadata.md) | Metadata read model, pane and overlays, safe writes, RAW sidecars |
| [07-photo-editing.md](07-photo-editing.md) | EditStack, evaluation, RAW editing, export |
| [08-video-editing.md](08-video-editing.md) | Two-path trim, rotate, split, remux and extract |
| [09-build-and-test.md](09-build-and-test.md) | Build system, test suites, gates, CI, Windows shell integration |
| [10-roadmap.md](10-roadmap.md) | Feature inventory by PR number, with each PR's verify line |
| [11-licensing.md](11-licensing.md) | Licence, third-party linkage, codec patents |
| [12-decision-log.md](12-decision-log.md) | Why current behaviour is the way it is, by topic and date |
| [13-updates-and-telemetry.md](13-updates-and-telemetry.md) | Install, updates, crash reporting, telemetry, privacy |
| [14-abi.md](14-abi.md) | The C ABI between the hosts and the core |
| [15-platforms.md](15-platforms.md) | Windows and macOS hosts over one core |
| [16-commands.md](16-commands.md) | Command table, key router, modes, default key map |
| [17-local-ai-search.md](17-local-ai-search.md) | Local AI search add-on |
| [18-import.md](18-import.md) | Import add-on and how add-ons install |
| [19-voice.md](19-voice.md) | Voice query (not built beyond the query parser) |
| [20-edit-workspace.md](20-edit-workspace.md) | The Edit workspace |
| [21-video-editor.md](21-video-editor.md) | The Video Editor window |
| [22-editor-addon.md](22-editor-addon.md) | The Editor add-on (not built) |
| [23-nle-search.md](23-nle-search.md) | Local search inside Final Cut Pro; FCPXML export |
| [24-transfer.md](24-transfer.md) | Network-speed verified copies |
