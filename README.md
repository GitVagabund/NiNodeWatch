# NiNodeWatch

NiNodeWatch is an SKSE crash-diagnostics plugin for Skyrim Special Edition 1.5.97.

It monitors NiNode / scene graph operations and records detailed context around crashes involving child-node processing, NiTObjectArray operations, and NIF scene graph traversal.

The plugin is intended for difficult CTDs where conventional crash logs do not clearly identify the problematic mesh or scene node.

## Supported version

- Skyrim Special Edition 1.5.97
- SKSE64 for Skyrim 1.5.97

NiNodeWatch is currently built specifically for Skyrim SE 1.5.97.

## What it does

NiNodeWatch hooks several NiNode / NiTObjectArray operations and records:

- Node names
- Child pointers
- Array state
- Capacity / size / free index
- Thread IDs
- Stack traces
- Scene graph context
- Active NIF-related node names
- Access violations occurring during NiNode processing

When a relevant exception occurs, NiNodeWatch writes an incident log that can help identify the mesh or scene node involved.

## Real-world example

NiNodeWatch was originally developed while investigating a reproducible crash in the Soul Cairn.

Standard crash logs were not sufficient to identify the cause.

NiNodeWatch captured scene graph activity immediately before the crash and repeatedly identified:

`dlcsoulcairnwisp.nif`

and:

`DlcSoulCairnWisp`

during NiNode child processing.

Disabling the placed Whispering Spirit references eliminated the reproducible CTD during subsequent testing.

This does not necessarily mean that the vanilla mesh itself is universally broken. NiNodeWatch should be treated as a diagnostic tool that helps identify scene graph context around a crash.

## Installation

Install the release archive using Mod Organizer 2 or another mod manager.

The plugin DLL should end up in:

`Data/SKSE/Plugins/NiNodeWatch.dll`

Launch Skyrim through SKSE as usual.

## Logs

NiNodeWatch writes diagnostic and incident logs when relevant scene graph activity or exceptions are detected.

Incident logs use names similar to:

`NiNodeWatch-incident-YYYYMMDD-HHMMSS-....log`

These logs contain the recent NiNode operation history leading up to the detected exception.

## Example log data

Typical entries include:

```text
SetAt1 ENTER
node=...
array=...
name="dlcsoulcairnwisp.nif"
child=...
index=1

children:
readable=1
capacity=2
freeIdx=1
size=1
```
The most useful fields when diagnosing a crash are usually:

name
focusNode
exception address
recent ENTER / LEAVE operations
repeated NIF or node names immediately before the crash
Important

NiNodeWatch is a diagnostic plugin.

It does not automatically repair broken meshes, plugins, scripts, or scene graphs.

The recorded node or NIF name should be treated as a strong diagnostic lead, not absolute proof that the named asset is inherently defective.

Building from source

The project uses CMake and vcpkg.

Required project files include:

CMakeLists.txt
vcpkg.json
vcpkg-configuration.json

A PowerShell build helper is also included:

build.ps1

License

License information will be added separately.

## Russian documentation

See:

[README_RU.md](README_RU.md)
