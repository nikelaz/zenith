# Application structure

The main loop handles platform events, collects provider results and updates
conversations, collects UI service results, then draws and presents the windows.

- `state/chat.h` defines persisted chat data. Assistant text and tool calls use
  ordered segments. User messages use `content`.
- `conversation/` owns active turns, cancellation, event application, request
  construction, and title generation. It does not depend on SDL or ImGui.
- `providers/` translates native protocols into events. The function table is
  the provider boundary; protocol parsing remains specific to each adapter.
- `ProviderRuntime` dispatches turns concurrently and joins completed workers
  while running. Callers assign nonzero turn IDs. Shared process tracking and
  startup, usage, and skill state are concrete structs.
- `McpService` owns its worker and task/result queues. The application starts and
  stops it; the UI owns editing drafts and feedback presentation.
- `ui/` owns composers and view state. Model options live in `ChatThread`.
  Selection endpoints belong to each panel; text spans and markdown buffers are
  reusable rendering scratch shared across panels. Skill lists are borrowed
  from snapshots for the duration of rendering.
- `persistence/` normalizes legacy assistant text when loading. Saving derives
  the plain text column from segments, preserving the existing database format.

Conversation IDs identify threads across insertion, deletion, and reordering.
Asynchronous attachment results retain the originating conversation ID.
Provider events must match the active conversation, turn, and provider. Each
turn owns its assistant message, so cancelled work cannot append to a newer
response. Request history owns role/text snapshots independently of UI data.

Shutdown first requests provider cancellation, then joins MCP work, releases UI
resources, saves state, destroys providers, and closes the platform resources.
Providers and their function tables remain alive until all service work has
joined. Existing scoped standard-library locks and callback queue ownership
remain in use; resource orchestration uses explicit start/shutdown functions.
