# Flowie Workbench

A native Windows protocol test client built with [OpenWTL](https://github.com/qigao/OpenWTL/tree/v0.1.2).
One workspace hosts up to eight independent tabs for MQTT, HTTP/HTTPS, JSON-RPC,
raw TCP or serial. Each tab has its own draft and result history. Open a protocol
from **New Request** in the toolbar or **File → New Request**.

## Protocols

| Tool | Request | Output |
| --- | --- | --- |
| MQTT | Choose Publisher, Subscriber or Client in the connection dialog. | Messages, acknowledgement reasons and connection history. |
| HTTP/HTTPS | Enter an absolute URL, choose a method and optionally add headers and a UTF-8 body. HTTPS uses the Windows trust store and checks the server identity. | Status, response headers and body. |
| JSON-RPC | Enter an HTTP or HTTPS endpoint, a method and optional JSON object or array parameters. Each send makes one JSON-RPC 2.0 call. | Correlated result or remote error, with the original response. |
| TCP | Enter `tcp://host:port`. Connect, then send UTF-8 text or hexadecimal byte pairs. | The timestamped byte stream, with binary events displayed in hex. |
| Serial | Refresh or enter a COM port. Choose baud rate, data bits, parity, stop bits and flow control. Open the port, then send text or hexadecimal byte pairs. | The timestamped byte stream. |

HTTP and JSON-RPC have a 10-second request deadline, up to 8 KiB of headers and up
to 64 KiB per encoded body. TCP and Serial accept up to 64 KiB in the composer.

## Workspace commands

- **New Request** and **Ctrl+N** open the protocol picker. MQTT includes Client,
  Publisher, Subscriber and **Load Connection Profile**.
- The toolbar's primary action adapts to the selected tab. It connects MQTT and TCP,
  opens a serial port, then offers **Publish** or **Send Bytes** where applicable.
  HTTP and JSON-RPC use **Send Request** and **Send RPC Call**. The adjacent control
  disconnects, closes a port or cancels an in-flight request.
- **Session** offers connection and stop commands, serial port refresh and recent
  MQTT connections. **Request** and the toolbar's **Actions…** menu offer send and
  payload tools. **Ctrl+Enter** sends and **Ctrl+D** stops the active operation.
- **View → Workspace / Editor / Output** switches the selected tab's layout. Each
  tab remembers its layout while its draft and I/O continue to belong to that tab.

## MQTT

- The MQTT connection dialog can connect immediately or open a new disconnected tab.
- Recent Connections retains up to 12 distinct successful connections across launches.
  Selecting one under Session → Recent MQTT Connections opens a prefilled dialog for its original role.
  The picker in any new connection dialog can reuse an endpoint for the role already
  chosen. History saves the host, port, transport, MQTT version and role. Reenter
  credentials, TLS file paths and any custom WebSocket path when needed.
- Each tab has a unique default client ID. Publisher tabs send, Subscriber tabs subscribe,
  and Client tabs support both. Select a tab to edit its controls and inspect its messages.
  A fixed connection bar keeps the broker and Connect or Disconnect above the request.
  Message, Topics, Authorization and Settings share the request pane. Editor opens
  Message for a Publisher or Client and Topics for a Subscriber. Output expands the
  message stream and original payload.
  Ctrl+PageUp / Ctrl+PageDown switches the active client. Each tab has its own ×
  button, including background clients; Ctrl+W closes the active client.
  Use Save Request in the toolbar or Ctrl+S to save the active tab. Double-click
  a saved item in Collections or press Enter to restore it in a new tab.
  Client panels scroll vertically on short or scaled displays; the workspace toolbar
  and client tabs stay visible.
  Background tabs continue receiving and maintaining keepalive. Closing a tab drains
  just that connection and disables its close button until cleanup finishes.
  Closing the main window drains all clients.
- TCP, TLS, WS and WSS; MQTT 3.1.1 and 5, clean sessions and a 30-second keepalive.
- Subscribe/unsubscribe with QoS 0–2; publish with QoS 0–2 and retained messages.
  Active subscriptions are committed after successful ACKs (128 filters maximum).
  Reserved topics are supported under `$SYS/`; other `$` namespaces and shared
  subscriptions report unsupported rather than accepting an unobservable stream.
- UTF-8 text and JSON payloads; validation and explicit formatting use the existing
  SaltsBase JSON parser. Publishing JSON validates without rewriting it.
- Timestamped receive, transmit and operation records, with separate stream filters.
  Non-text payloads are shown as hexadecimal; details preserve the original bytes.
- Credentials stay in memory and are never saved to a profile or event history.
  TLS verification stays enabled; custom CA and client certificate/key are supported.
  A 15-second idle PING maintains the 30-second keepalive. No automatic reconnect
  or publication replay. Enhanced AUTH, encrypted private-key passwords and MQTT
  property editing are outside this first release.

## Build and run

Load `.env` through the existing native SDK restore entry point, then configure/build
with `native-release-user`. Windows `.env` selects `OPENWTL_VERSION` and
`OPENWTL_SHA256` (see `.env.example`). CMake FetchContent verifies that archive and
links only `OpenWTL::WTL`; it does not search for another ATL provider. This
header-only library supplies the Windows GUI framework absent from the client SDK.
Version 0.1.2 supports LLVM-MinGW and MSVC, with Apache-2.0 OpenATL and MS-PL WTL
sources. No Scintilla or second GUI toolkit is required.

Run `build/native-release-user/bin/flowie_mqtt_workbench.exe`. Start a local broker,
choose New Request → MQTT → Client, enter host/port, and choose Connect. Subscribe to `workbench/#`,
then publish to `workbench/test`.
For multiple clients, add a Subscriber and connect/subscribe it first, then add one
or more Publishers to the same broker/topic. Switch between tabs to inspect their
independent RX/TX counts. Different host/port settings test separate brokers without
disconnecting the other tabs.
Choose JSON to validate before publishing. App tests run through
`ctest --preset native-release-user -R test_flowie_workbench --output-on-failure`.
The native window suite also writes `workbench-http.bmp`, `workbench-studio.bmp`,
`workbench-subscriptions.bmp` and `workbench-command-focus.bmp` into its build-side
test working directory for visual inspection. The publish and monitor views also have
dedicated previews. These render the actual shared UI
implementation, including connected message data, native selection and command focus.

## Load files

**New Request → MQTT → Load Connection Profile…** opens a UTF-8 JSON profile (up to 64 KiB) in the connection
dialog. For example, save this as `local-broker.json`:

```json
{
  "role": "client",
  "host": "127.0.0.1",
  "port": 1883,
  "transport": "tcp",
  "protocol": "5"
}
```

`host` is required. The defaults are `client`, port `1883`, `tcp`, and MQTT `5`.
Roles are `client`, `publisher`, and `subscriber`. Transports are `tcp`, `tls`, `ws`,
and `wss`. MQTT `3.1.1` is also accepted. Optional keys are `path` (default `/mqtt`),
`username`, `ca_file`, `cert_file`, and `key_file`. Relative TLS paths are resolved
from the profile's directory. Enter the password in the dialog before selecting Connect.

**Request → Load Sample…** loads into the active request's editor. HTTP, JSON-RPC,
TCP and Serial accept UTF-8 text up to 64 KiB. JSON-RPC samples must contain a JSON
object or array. TCP and Serial switch to text input when loading a sample.

For an MQTT Publisher or Client, a UTF-8
`.txt` file supplies text and `.json` supplies a validated raw JSON payload. Both keep the
current topic, QoS and retain setting. A file ending in `.mqtt-sample.json` can set
those fields together. Save this as `sensor.mqtt-sample.json`:

```json
{
  "topic": "workbench/sensor",
  "qos": 1,
  "retain": false,
  "format": "json",
  "payload": "{\"device\":\"sensor-17\",\"online\":true}"
}
```

The sample's `payload` is a string. For plain text, set `format` to `text`; the
defaults are `text`, QoS 0 and retain off. The draft limit is 960 KiB, and structured
sample files are capped at 2 MiB. A leading UTF-8 BOM is removed. Choose Publish
explicitly to send the loaded draft.

## Collections and history

Use **Open Collections…** on the toolbar or under **File** to load a Workbench
collection JSON file. The **Collections** sidebar groups requests by their file and
collection name. Select a request and press Enter or double-click to open it in
a new tab. **History** lists recent successful MQTT connections. Open an entry to
review its settings in the connection dialog.

**Manage** in the sidebar opens a separate workbench tab, also available under
**File → Manage Collections**. It shows the full paths and request counts of the
loaded files. **Reload** reads the selected file from disk. **Unload** removes a
file from the current workspace. The file stays on disk and open request tabs
keep their drafts. **View → Sidebar** hides or restores the navigation.

**Save Request** in the toolbar or **File** takes a snapshot of the active request
tab. The dialog names the destination file. An opened request saves to its original
file while that file is loaded. For a new request, select a file or any of its entries
in the sidebar before saving. With no selection, the last file in the sidebar is
used. With no loaded files, saving uses the default library. Type a new collection
name or select an existing one, then give the request a name. Replacing an existing
request asks for confirmation.

A saved MQTT request keeps the role, broker endpoint, protocol, transport, TLS paths,
and WebSocket path without query parameters. It also keeps the publish draft
(topic, QoS, retain, format and payload) and subscription presets. The presets
include the current subscription filter, active subscriptions, and any presets
already in the tab. Username, password and the generated client ID are excluded.
Message payloads are saved as entered.

HTTP, JSON-RPC, TCP and Serial entries keep their protocol, endpoint, body and
applicable options. HTTP headers, URL query parameters and fragments are excluded
from the file. Request bodies are saved as entered.

Opening a request creates an idle tab. For MQTT, supply any credentials and choose
**Connect**. On the **Topics** page, choose a **Saved topic** to fill the filter
and QoS, then choose **Subscribe**. Use **Publish** to send the restored draft.
Saved requests are independent of the Recent Connections list.

The default library is `%LOCALAPPDATA%\Flowie\MQTT Workbench\collections.json`.
An existing default library opens at startup. Other files are loaded for the current
workspace session. The workspace supports up to 16 loaded files with up to 64 requests
and 8 MiB in each file.
Each request supports up to 128 saved topics. The library owns the persisted snapshots,
while each open tab owns its editable draft and MQTT session. A save validates the
full replacement before committing the file. A concurrent write or a size limit
reports an error and keeps the prior file. Existing MQTT entries load without a
migration. A library with other protocol entries requires a build with multi-protocol
support. To use an older Workbench, retain a copy of the MQTT-only library.

## Ownership and failure protocol

### Native Studio layout

The UI uses TurboDB Studio's palette, native tabs and resizable WTL panes. The request
area follows the information hierarchy of the
[Postman MQTT interface](https://learning.postman.com/docs/use/send-requests/protocols/mqtt-client/mqtt-request-interface).
Collections, History and Manage share the left navigation. To the right, request and
management tabs share the same tab strip. The MQTT broker bar sits above an editor
that switches between Message, Topics, Authorization and Settings. Publisher and
Subscriber tabs expose their respective request pages. WebSocket
and TLS options appear on Settings as needed. The message stream and original payload
sit below the editor. A filter changes the visible rows without changing message history.
The connection bar and status strip remain visible while request pages scroll or the
message stream expands. Standard Windows inputs keep native focus, selection, disabled
and password behavior. High Contrast uses system colors.
Tab close controls are native keyboard-focusable buttons with client-specific
accessible names and tooltips. They follow the native tab geometry and stay clear
of overflow scroll arrows; closing an inactive tab preserves the selected client.

The Studio palette lives in `src/ui/theme.cpp`. White (`#FFFFFF`) is used for
content and selected tabs, light gray (`#F1F1F1`) for tab strips, inactive tabs,
section headers and the status bar, and charcoal (`#4C4C4C`) for the command bar.
Both tab strips and their close buttons use the same palette. The native controls
still handle focus, selection, keyboard input and tab overflow.
The menu bar uses OpenWTL `CMenu`, file selection uses `CShellFileOpenDialog`, and
task views use the existing WTL splitter's single-pane mode. Moving between views
keeps the same child controls, drafts, history and MQTT session. This preserves
splitter positions when returning to Workspace and avoids a second set of windows
requiring synchronization with the tab.

The workspace owns one validated snapshot per loaded collection file. The sidebar and
management list read these snapshots, and each opened request copies its editable draft.
Loading or reloading a file validates the entire replacement before publishing it.
Keeping files separate preserves same-named requests from different files and lets
the existing atomic save operate on the selected file. A merged import would have
needed collision and write-back rules. The collection format stays compatible with
previous builds that support the protocols stored in the file.

Each protocol pane exposes the name and availability of its request actions.
The workbench uses those capabilities for the main menus, toolbar and shortcuts.
The pane rechecks availability when a command runs and continues to own its draft,
session and results. The toolbar remembers which primary action it displayed, so
a state change before a click cannot turn Send into Connect. Workbench commands
reuse the pane operations that the local controls call. This keeps admission in one
place and lets each new protocol supply its own labels and state rules.

The palette and pane composition are local UI concerns. Flowie does not include
sibling source paths or depend on TurboDB Studio's private widgets, Scintilla or
Lexilla. Native pane notifications are forwarded synchronously on the UI thread;
their borrowed notification pointers never escape the call. The existing Session,
mailbox, history and shutdown contracts below remain the state owners.

The host owns up to eight request windows and one Collections manager. Each request
window owns its controls, draft, session and history. Tab changes show or hide the same
child windows, so background tabs keep polling their own mailboxes. A shared form would
need to snapshot credentials and drafts on every selection. Separate top-level windows would each need menu, save
and close coordination. Stable child windows reuse the existing host and bound the
additional controls and workers by the tab limit.

The protocol tabs use the selected CHttp, Salts CNet and Salts Serial SDKs. For each
request or connection, a worker owns the SDK client, advances I/O and copies borrowed
callback data into the bounded Workbench mailbox. It admits one stream write at a
time. HTTP cancellation, CNet stop and serial close complete on that owner before
the UI joins it. The worker also handles serial reads and writes with finite timeouts.
This shares collection snapshots, history bounds and tab ownership with MQTT while
giving each tool its own editor and result display.

### MQTT session lifecycle

Each Session retains the existing single-command admission contract; independent
Sessions may have commands in flight concurrently. Role checks also apply at command
dispatch and in the controls. Connection attempts start only on Connect.

The workspace owns connection history separately from each tab's message history.
The dialog borrows a snapshot of recent endpoints for its modal lifetime. A tab supplies
its endpoint to the workspace on the UI thread after a successful CONNACK. The workspace
stores the newest entry first in the `Recent Connections` value under the current user's
`Software\\Flowie\\MQTT Workbench` registry key. Reusing the same endpoint and role
moves it to the front. Canceled dialogs and rejected connections leave history unchanged.
Using one bounded registry value keeps a consistent list and avoids a separate profile
file or a broker-side dependency. Older builds can run without reading the value.

Child windows destroy themselves only after their own Session reaches idle. The host
observes destruction on its UI timer and then releases the child's storage. It never
erases a live callback context. Closing a child does not post WM_QUIT; only the host
posts it after all children have drained. Failed close remains visible and retryable,
and switching to another client never abandons the failed client. Empty workspaces
allow adding clients again. The eight-client limit bounds mailbox/history retention
to 8 × (4 + 8) = 96 MiB, excluding SDK buffers, controls and temporary formatting.

The UI owns controls, connection state and history. It calls the SDK's bounded,
asynchronous command API directly and never runs network I/O. Only one command is
pending per connection at a time, so response identity is unambiguous. Admission failures leave
the command uncommitted and are displayed immediately. Broker ACK reasons are
displayed separately from local status codes.

SDK callbacks borrow packet views only during the callback. They copy topic,
payload and ACK reasons into an MPSC mailbox consumed by the UI's 50 ms timer.
The mailbox is bounded to 256 records and 4 MiB of dynamic bytes. Each payload is
bounded by the 1 MiB SDK packet limit. Full/allocation failure latches a terminal
error outside the queue; inbound callbacks return it and the UI closes the session.
There is no silent overflow or unbounded callback allocation.

History is a separate 500-record/8 MiB rolling view. Oldest records are explicitly
evicted and a visible counter reports removals. Clearing releases retained payloads.
Detail formatting never changes the original bytes.

Disconnect/close stops UI admission and transfers exclusive client access to a
joinable destruction thread. `try_destroy(30000)` interrupts I/O and drains the
SDK. A separate atomic completion slot cannot be lost when the mailbox is full.
The UI drains callbacks and joins that thread before releasing the callback
context or allowing another connection. Failed destruction keeps the window and
context alive and exposes Retry close. Closing the window waits asynchronously for
the same protocol. Callback exceptions never cross the C ABI; callbacks never
touch HWNDs.

The extension is contained in the Windows application and its private model.
Rolling back the executable requires the MQTT-only library copy described above.

Windows CI's `FLOWIE_BUILD_ENV_WINDOWS` must include the same OpenWTL version and
SHA-256 fields. Build and normal install copy the application's transitive DLLs
and the selected LLVM-MinGW C++ runtime into `bin`; launching needs no SDK PATH.
