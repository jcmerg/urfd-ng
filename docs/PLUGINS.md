# Protocol Plugins (draft)

> Work in progress.

## Goal

A protocol need not live in the urfd source tree. At start urfd loads shared
objects (`.so`) that bring one protocol each. The core knows no single plugin:
the ini section, the protocol name, the admin commands and the sections on the
admin dashboard all come from the plugin.

## Loading

```ini
[Plugins]
Load = /usr/local/lib/urfd/foo.so
Load = /usr/local/lib/urfd/bar.so
```

- The plugins are loaded when `urfd.ini` is read the first time
  (`CConfigure::PluginHook`, set in `Main.cpp`), before the protocols start.
- A reload (SIGHUP) hands the plugins their sections again but loads no new
  `.so`. A new `Load` line counts only after a restart.
- Nothing is unloaded; the objects stay loaded until urfd ends.
- An error while loading (missing file, wrong ABI, duplicate name) stops urfd
  with a message, like an error in the ini file.

## Bound to its urfd (ABI)

A plugin is C++ built against exactly this source tree: it derives from
`CProtocol` and `CClient` and uses `g_Reflector`, `g_Configure`,
`g_GateKeeper`, `g_Credentials` and so on. urfd is linked with `-rdynamic` for
this.

So that a plugin does not work on objects it knows differently, urfd compares
on loading (`SPluginAbi` in `reflector/Plugin.h`):

- the plugin API version (`URFD_PLUGIN_API`)
- the urfd version (`URFD_VERSION_*` in `reflector/Version.h`)
- the build options that change classes (`NO_DHT`, `DEBUG`)
- the size of the main classes (`CReflector`, `CProtocol`, `CClient`,
  `CConfigure`, the packets, `CPacketStream`, `SJsonKeys`)

As a result a plugin has to be rebuilt with every urfd update.

Open:

- A check beyond the sizes, such as a hash of the headers?
- The same compiler and the same libstdc++ are assumed, but not checked.

## Interface (`CPlugin`)

| Method | Purpose |
|---|---|
| `GetName()` | name = ini section, the name in `BlockProtocols`, on the admin socket and the dashboard |
| `GetDescription()` | one line for the protocol list of the dashboard |
| `Configure(cfg, section)` | reads its ini section (lines in file order) and stores values with `cfg.Set()` under keys of its own; `true` = error |
| `IsEnabled()` | whether the protocol is started |
| `CreateProtocol()` | the protocol; urfd then calls `Initialize(GetType(), id, GetPort(), HasIPv4(), HasIPv6())` |
| `Reload(proto)` | after a SIGHUP |
| `AdminCommand(proto, command, cmd, result)` | its own admin socket commands |
| `AdminStatus(proto, status)` | its own fields in the answer to `status` |
| `AdminMappings(proto, mappings)` | its own entries in `tg_list` |
| `AdminUi()` | the description of its sections on the admin dashboard (see below) |

A plugin exports itself with `URFD_PLUGIN(CFooPlugin)`.

### Protocol ID

Each plugin gets an `EProtocol` of its own after `EProtocol::plugin`
(`PluginProtocolId(i)`, `IsPluginProtocol(p)`). Its clients return it from
`GetProtocol()`; the protocol knows it as `m_ProtocolType` after `Initialize`.
The gatekeeper treats plugin clients like nodes (`IsNodeListedOk`) in
`MayLink` and `MayTransmit`.

### Logins

`ECredService` takes any service name of 1 to 16 lower case letters and
digits, so a plugin can keep its logins in the credential database with
`ECredService("foo")`.

## Admin Socket

- Its own commands work by their name (`{"cmd": "foo_list", ...}`) or aimed at
  one plugin:
  `{"cmd": "plugin", "name": "Foo", "command": "foo_list", ...}`.
- `status` contains `plugins`: `[{name, description, active, ui}]`, plus the
  fields from `AdminStatus()`.
- `tg_list` with `"protocol": "foo"` (the name in lower case) or `"all"`.

## Admin Dashboard

The dashboard holds nothing specific to a plugin. For each active plugin it
draws the sections from `status.plugins[].ui` into an area of the admin page of
their own, with entries in the index. All calls go to the admin socket through
the `plugin` action of `api/admin.php`.

The description (`AdminUi()`) is an array of sections:

```json
[
  {
    "id": "users",
    "title": "Foo users",
    "list":   { "command": "foo_user_list", "rows": "users", "empty": "No users" },
    "columns": [
      { "key": "name",  "label": "Name", "source": "source" },
      { "key": "since", "label": "Since", "format": "time" }
    ],
    "form": {
      "command": "foo_user_add",
      "submit": "Add user",
      "fields": [
        { "name": "name", "label": "Name", "required": true, "width": "s" },
        { "name": "password", "label": "Password", "type": "password", "width": "m" }
      ]
    },
    "remove": { "command": "foo_user_remove", "key": "name", "arg": "name" }
  }
]
```

| Field | Meaning |
|---|---|
| `list.command` | the command that returns the rows; the dashboard calls it when the page opens, every 10 s and after each change |
| `list.rows` | the array in the answer |
| `columns[].format` | `text` (default), `list` (array, comma separated), `time` (Unix time), `bool` |
| `columns[].source` | a field holding `"db"`/`"ini"`; with `"db"` the cell gets "(database)" |
| `form` | input fields; submitting calls `command` with the fields as arguments |
| `remove` | a button per row; calls `command` with `{arg: row[key]}` |

Everything that comes from the plugin or from its answers is written into the
page escaped; the plugin supplies no HTML and no JavaScript.

Open:

- more building blocks: buttons without a row (e.g. "Reconnect"), select
  fields, a confirmation before removing
- sections of its own on the public pages (not only on the admin page)?

## Building

A plugin lives in a directory of its own with a `Makefile` that expects the
urfd sources in `URFD=` and takes over their `urfd.mk` (DHT, debug):

```sh
make -C plugins/foo URFD=$PWD/reflector
```

- Compiled with `-fPIC`, linked with `-shared`, without the urfd objects: the
  symbols come from the `urfd` executable at run time.
- Docker: every `plugins/<name>/` with a `Makefile` is built in the image and
  put into `/usr/local/lib/urfd/`.

## Limits

- one protocol per plugin
- loaded at start only, no loading or unloading at run time
- `inicheck` does not know plugin sections and only notes them
- License: urfd is GPLv3. A plugin runs in the same process and uses the
  classes of urfd. Whoever passes a plugin on (as a binary) most likely passes
  on a work that falls under the GPL as a whole; whoever only runs it
  themselves passes nothing on.
