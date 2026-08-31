# Firestorm automation

Firestorm exposes a single, platform-agnostic way to drive a running viewer from
the outside: **one automation vocabulary of verbs**, reached through
platform-native adapters. AppleScript on macOS is the first adapter; Windows and
Linux adapters are planned and will speak the *same* vocabulary.

---

## The common model

Everything goes through one entry point:

```
FSAutomation::call(verb, args)  ->  result
```

- **`verb`** is a short string naming an action (`"sendChat"`, `"teleport"`,
  `"getState"`, …).
- **`args`** is a structured map of named arguments.
- **`result`** is always a structured map:
  - success: `{ "ok": true, ...verb-specific payload... }`
  - failure: `{ "ok": false, "error": "<human-readable message>" }`

Contract details:

- **Main-loop execution.** Every verb runs on the viewer's main loop, interleaved
  with (never concurrent to) frame rendering. Adapters that are invoked on another
  thread are marshalled automatically; the caller blocks until the result is ready.
- **Login gating.** Verbs that touch the agent or the world return
  `{ "ok": false, "error": "Firestorm is not logged in yet" }` until login has
  completed. `getState` always answers (it reports `logged_in`).
- **In-process access.** The dispatcher is also an `LLEventAPI` on the
  `"FSAutomation"` pump, so LEAP plugins and other in-process code use the same
  verbs.

### Verb reference

| Verb | Arguments | Result payload |
|---|---|---|
| `sendChat` | `message`, `channel` (default `0`), `type` (`whisper` \| `normal` \| `shout`) | — |
| `sendIM` | `message`, `target` (avatar UUID) | — |
| `sendGroupIM` | `message`, `group` (group UUID) | — |
| `teleport` | `regionname` (optional), `x`, `y`, `z` | — |
| `teleportHome` | — | — |
| `getSetting` | `group` (default `"Global"`), `key` | `value`, `type` |
| `setSetting` | `group`, `key`, `value` | — |
| `dispatchCommand` | `command` (name or full `secondlife:///app/…` URL), `params` (list) | `handled` |
| `runGesture` | `name` | — |
| `callUIAction` | `name`, `parameter` (optional) | — |
| `getState` | — | `logged_in`, `region`, `avatar_name`, `avatar_legacy_name`, `avatar_key`, `position` `[x,y,z]` (global), `region_position` `[x,y,z]`, `location_string` |
| `getLocation` | — | `region`, `parcel`, `maturity`, `slurl`, `region_pos` `[x,y,z]`, `global_pos` `[x,y,z]`, `facing` `[x,y,z]` (avatar body forward, unit vector), `heading` (compass degrees CW from grid north, `0`–`360`) — one atomic snapshot for saving a landmark |
| `getNearbyAvatars` | `dist` (default: draw distance) | `avatars`: list of `{ name, id, global_pos, region_pos, distance }` |

The `verb` column is the dispatcher / LEAP name. The macOS AppleScript command
names differ for three of them so they don't shadow AppleScript's own `get`/`set`
verbs: `getSetting` → **`query setting`**, `setSetting` → **`change setting`**,
`getLocation` → **`capture location`**.

- `teleport` with a `regionname` treats `x`/`y`/`z` as region-local; without one,
  they are global coordinates. There is no confirmation dialog.
- `dispatchCommand` runs SLURL "app" commands as a trusted caller, so it is not
  subject to the throttling/blocking applied to web-page-originated SLURLs.

---

## macOS: AppleScript

The macOS build ships a scripting dictionary (`Firestorm.sdef`). Open it in
**Script Editor ▸ File ▸ Open Dictionary… ▸ Firestorm** to browse the full
terminology.

The first time a script talks to Firestorm, macOS shows an **Automation** consent
prompt ("… wants to control Firestorm"). Approve it once (or manage it later under
**System Settings ▸ Privacy & Security ▸ Automation**).

### Command examples

```applescript
tell application "Firestorm"

    -- chat
    send chat "hello from applescript"
    send chat "psst" as whisper
    send chat "9 8 7" on channel 42

    -- instant messages (UUIDs)
    send instant message "ping" to "11111111-2222-3333-4444-555555555555"
    send instant message "meeting in 5" to group "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"

    -- teleport
    teleport to region "Da Boom" x 128 y 128 z 25
    teleport x 275000 y 265000 z 40      -- global coordinates
    teleport home

    -- viewer settings (a.k.a. debug settings)
    set farClip to query setting "RenderFarClip"
    change setting "RenderFarClip" to 96

    -- run a SLURL app command / a gesture / a UI action
    dispatch command "agent" with parameters {"11111111-2222-3333-4444-555555555555", "about"}
    run gesture "/hello"
    call ui action "Tools.TakeSnapshot"

end tell
```

### Reading viewer state

```applescript
tell application "Firestorm"
    set here to current region
    set me to avatar name
    set pos to agent position          -- {x, y, z}
    set loc to location string

    -- read-only list of avatars in draw distance
    set names to name of every nearby avatar
    set closest to first nearby avatar whose distance = ¬
        (my min_of(distance of every nearby avatar))
end tell
```

Application properties: `logged in`, `current region`, `avatar name`,
`avatar legacy name`, `avatar key`, `agent position`, `agent region position`,
`location string`. Element: `nearby avatar` (`name`, `avatar key`,
`global position`, `region position`, `distance`).

### Saving and restoring external landmarks

`capture location` returns everything needed to come back later. Because a script
command can only hand AppleScript a **string**, the `as` parameter picks the
encoding — same fields either way: `region`, `parcel`, `maturity`, `SLURL` /
`slurl`, `region_position` `{x, y, z}`, `global_position` `{x, y, z}`,
`facing` `{x, y, z}` (the avatar body's unit forward vector) and `heading`
(compass degrees clockwise from grid north, `0`–`360`). `facing` / `heading`
are local body rotation, not the "look at" gaze target — the *Private look at*
settings do not affect them.

**Default — an AppleScript record literal.** Get the string, then materialise it
with `run script` for named-field access; this is the AppleScript-native
structured form. (You can't nest the `tell` inside `run script (…)` — grab the
string into a variable first.)

```applescript
tell application "Firestorm" to set raw to capture location
set L to run script raw
--> L = {region:"Shelter", parcel:"…", maturity:"General", SLURL:"…",
--       region_position:{125.883, 23.107, 3142.87},
--       global_position:{258941.883, 254743.107, 3142.87},
--       facing:{0.707107, 0.707107, 0.0}, heading:45.0}

tell application "Firestorm"
    teleport to region (region of L) ¬
        x (item 1 of region_position of L) ¬
        y (item 2 of region_position of L) ¬
        z (item 3 of region_position of L)
    -- region-independent: use global_position of L
    -- or: dispatch command (SLURL of L)
end tell
```

**`as json`** — a JSON object string. AppleScript has no JSON parser; use JXA or a
shell tool.

```sh
osascript -e 'tell application "Firestorm" to capture location as json' | jq .
osascript -l JavaScript \
  -e 'JSON.parse(Application("Firestorm").captureLocation({as:"json"})).region'
```

**`as lines`** — 14 newline-separated values, in order: region, parcel, maturity,
SLURL, region_x, region_y, region_z, global_x, global_y, global_z,
facing_x, facing_y, facing_z, heading.

```applescript
set L to paragraphs of (tell application "Firestorm" to capture location as lines)
tell application "Firestorm"
    teleport to region (item 1 of L) ¬
        x ((item 5 of L) as real) y ((item 6 of L) as real) z ((item 7 of L) as real)
end tell
```

The individual `agent position` (global) and `agent region position` (region-local)
properties are still there for one-off reads; `capture location` grabs them
together with the region name, parcel, maturity, the avatar's body facing
(`facing` / `heading`), and a portable SLURL in one call.

### From the shell / other languages

```sh
osascript -e 'tell application "Firestorm" to get current region'
osascript -e 'tell application "Firestorm" to send chat "built at $(date)"'
```

Any language that can send Apple Events works too (Python via `PyObjC` /
`ScriptingBridge`, JXA via `osascript -l JavaScript`, Shortcuts, Automator).

### Error handling

A failed command raises an AppleScript error whose message is the dispatcher's
`error` string:

```applescript
try
    tell application "Firestorm" to teleport to region "Nowhere At All"
on error errMsg number errNum
    display dialog "Firestorm said: " & errMsg
end try
```

---

## Windows: COM *(planned)*

A future Windows build will register a small COM automation object (and/or a
`WM_COPYDATA` message subtype) that forwards `(verb, args)` to
`FSAutomation::call` and returns the structured result. The verb vocabulary and
result contract will be identical to the table above. Sketch:

```vbscript
Set fs = CreateObject("Firestorm.Automation")
fs.Call "sendChat", "{""message"":""hello from COM""}"
```

_Not implemented yet — this section is a placeholder for the planned adapter._

## Linux: D-Bus *(planned)*

A future Linux build will extend the existing `viewer_app_api` D-Bus interface
(currently SLURL-only) with a generic `Dispatch(verb, argsJson) -> resultJson`
method backed by `FSAutomation::call`. Sketch:

```sh
dbus-send --session --print-reply \
  --dest=com.secondlife.ViewerAppAPIService \
  /com/secondlife/ViewerAppAPI \
  com.secondlife.ViewerAppAPI.Dispatch \
  string:'sendChat' string:'{"message":"hello from dbus"}'
```

_Not implemented yet — this section is a placeholder for the planned adapter._

---

## For developers

- Dispatcher: `indra/newview/fsautomation.{h,cpp}` — add a verb by adding a
  `verbXxx()` handler and listing its name in the `verbs[]` table.
- macOS adapter: `indra/newview/Firestorm.sdef` (dictionary),
  `indra/newview/fsapplescriptcommands.mm` (Cocoa scripting glue),
  `indra/newview/fsapplescriptadapter.{h,cpp}` (LLSD marshalling shim).
- New OS adapters should call `FSAutomation::call()` only; they must not
  reimplement verb logic.
