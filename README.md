# PVLink

PVLink streams live ParaView geometry and colormaps into Unity and Unreal
Engine, using ParaView's own color-mapping rather than a separate design UI.
A ParaView plugin triangulates and sends whatever's in the pipeline; a relay
process (the DataManager) fans it out to any number of connected Unity/
Unreal clients, can record it to disk, and can replay a recording later with
no ParaView involved at all.

## Architecture

```
ParaView (PVLink.py plugin)
      │  TCP, dials out to --listen-port (default 9000)
      ▼
DataManager (DataManager/datamanager.py)
      │  TCP, clients dial IN to --client-port (default 9010)
      │  UDP, broadcasts presence on --discovery-port (default 9011)
      ▼
Unity / Unreal clients (dial out to the DataManager, or auto-discover it)
```

The DataManager is the hub. It never talks to Unity/Unreal directly except
by relaying what ParaView sends it (or, in replay mode, what's on disk) —
Unity and Unreal don't know or care whether they're watching a live
ParaView session or a recorded one.

Clients (Unity/Unreal) are always the ones dialing the DataManager, not the
other way around — this lets them join and leave freely with no DataManager
restart and no advance knowledge of who's connecting. A newly-joined client
is caught up to current state (project name, colormaps, bounds, time,
visibility, every mesh's latest geometry) before it starts receiving live
updates, so it doesn't just see a blank scene until the next real update.

If a client's DataManager address isn't configured, it listens for the UDP
discovery broadcast and auto-connects if exactly one DataManager is found on
the LAN — see "Discovery" below.

## Repo layout

| Path | What it is |
|---|---|
| `PVLink.py` | The ParaView plugin: `PVLinkMeshSenderFilter` (triangulates + sends one mesh) and `PVLinkDomainBoundsFilter` (sends bounds/time/project, owns the connection settings). Place a `PVLinkDomainBoundsFilter` upstream of every `PVLinkMeshSenderFilter` in a pipeline. |
| `DataManager/datamanager.py` | The relay/cache/replay/discovery hub. See its module docstring for full CLI usage. |
| `DataManager/.vscode/launch.json` | VS Code debug configs for running the DataManager in live or replay mode. |
| `ParaViewLinkUnreal/` | The Unreal plugin source (`Source/ParaViewLink/`), referenced by `Demos/Unreal/` via `AdditionalPluginDirectories` rather than being copied into the project. |
| `ParaViewLinkUnity/` | The Unity package source (`Runtime/`, `Editor/`), referenced by `Demos/Unity/`. |
| `Demos/Unreal/` | A demo UE5 project (`PVLink.uproject`) — an XR template project with the plugin wired in and a `SimContainerActor`/menu scene. |
| `Demos/Unity/` | A demo Unity project (URP) with a `SimContainer` prefab holding the receiver components. |
| `init.pvsm`, `anim.pvsm`, `run.pvsm` | Saved ParaView pipeline states for the demo `Wavelet → Contour/Slice/Sphere` pipeline with the PVLink filters wired in. |
| `setup.sh` | The author's personal `sshfs` mount helper for the Mac this repo is also hosted on — not a project setup script. |

## Quickstart

**1. Load the ParaView plugin.** Tools → Manage Plugins → Load New… → select
`PVLink.py`, tick Auto Load. Reminder: this plugin binds to ParaView's proxy
manager at *load* time — after editing `PVLink.py`, you need a fresh plugin
load or a ParaView restart, not just re-running something.

**2. Add the PVLink filters to your pipeline:**
  1. Load or open whatever data you want to stream, and hit Apply so it's a
     real source in the Pipeline Browser (a `Wavelet` source is a quick way
     to try this without real data).
  2. With that source selected, **Filters → PVLink → PVLink Domain Bounds**.
     In the Properties panel, set **Host**/**TCPPort** to match your
     DataManager (defaults `127.0.0.1`/`9000`, matching its `--listen-port`
     default) and **ProjectName** if you want this stream recorded/replayed
     under a specific name later. Apply. Place exactly one of these per
     pipeline — it's what owns the connection and sends `BOUNDS`/`TIME`/
     `PROJECT`.
  3. With that filter (or anything downstream of it) selected, **Filters →
     PVLink → PVLink Mesh Sender**. Set **MeshName** (the actor/object name
     it'll appear as on the receiver side — use a different name per mesh
     sender if you're streaming more than one), and **ColorArrayName**
     (leave blank to auto-detect whatever's actively coloring it in
     ParaView). Apply.
  4. Repeat step 3 for any other filter output you want to stream in the
     same pipeline (e.g. a `Contour` and a `Slice` off the same source) —
     each just needs to be downstream of the *same* PVLink Domain Bounds
     filter, which it'll find automatically and inherit the Host/TCPPort
     from, so you only ever configure the connection once per pipeline.
  5. From here it's automatic: every pipeline update (Apply, an animation
     `Play()`, a parameter change) sends fresh data through. Multiple mesh
     senders updating in the same real pipeline cycle flip together on the
     receiver side rather than landing on separate frames (see
     Backpressure below).

**3. Start the DataManager**, either live (waits for ParaView) or replaying a
previous recording (no ParaView needed):

```bash
# Live: relay ParaView, clients dial into 9010, also record to disk
python DataManager/datamanager.py --listen-port 9000 --client-port 9010 \
                                   --cache-dir ./recordings

# Replay: no ParaView needed, serve a previously-recorded project on loop
python DataManager/datamanager.py --client-port 9010 \
                                   --cache-dir ./recordings --project Sphere
```

**4. Open a client.** Either demo project's receiver dials the DataManager
using `DataManagerHost`/`DataManagerPort` (Unreal: `DefaultGame.ini`'s
`[/Script/ParaViewLink.SocketReceiverSubsystem]` section; Unity: the
`MeshReceiver` component's Inspector fields). Leave the host empty to use
auto-discovery instead (see below). Enter Play/PIE.

## Incorporating into your own UE/Unreal or Unity project

The steps below add the receiver to a project other than `Demos/Unreal`/
`Demos/Unity` — e.g. an existing app you want to bring live ParaView data
into. Nothing here needs the `Demos/` projects at all.

### Unreal

1. In your `.uproject`, add (or extend) `AdditionalPluginDirectories` with a
   path to the **directory that contains** `ParaViewLinkUnreal/` (not the
   plugin folder itself) — e.g. if this repo sits next to your project,
   `"../PVLink"`. Then enable the plugin:
   ```json
   "AdditionalPluginDirectories": ["<path-containing-ParaViewLinkUnreal>"],
   "Plugins": [{ "Name": "ParaViewLink", "Enabled": true }]
   ```
2. Rebuild (or open the Editor, which will offer to build it).
3. Drag a **Sim Container** actor (`ASimContainerActor`) into your level and
   size its box to cover the region you want ParaView data to appear in —
   it's found automatically via a `SimContainer` tag it sets on itself, and
   the coordinate transform (ParaView bounds → UE world space) is computed
   from it the first time a `BOUNDS` message arrives. Multiple instances are
   allowed; the first one found is used.
4. That's it for a minimal setup. `UMeshReceiverSubsystem` is a
   `GameInstanceSubsystem` — it's created automatically, dials the
   DataManager on its own (or auto-discovers it if `DataManagerHost` is
   left empty), and generates its own scalar-field material
   (`M_ScalarField`) the first time it's needed, saving it into
   `ParaViewLinkUnreal/Content/` so it persists across sessions. No manual
   material assignment or Blueprint subclassing required.
5. Optional: set `DataManagerHost`/`DataManagerPort`/`DiscoveryPort` in your
   project's `Config/DefaultGame.ini` under
   `[/Script/ParaViewLink.SocketReceiverSubsystem]` (see the Ports table
   below for defaults) if you don't want auto-discovery.

### Unity

1. Add a local file reference to the package in your project's
   `Packages/manifest.json` dependencies, pointing at wherever this repo's
   `ParaViewLinkUnity/` folder lives relative to your project:
   ```json
   "com.paraviewlink.unity": "file:../relative/path/to/ParaViewLinkUnity"
   ```
   (`Demos/Unity` itself uses `file:../../../ParaViewLinkUnity` — adjust for
   your project's location relative to this repo.)
2. Add a `MeshReceiver` component (Add Component → ParaViewLink → Mesh
   Receiver) to a GameObject in your scene. Assign `BaseScalarMaterial` —
   unlike Unreal, Unity doesn't generate this automatically (leaving it
   unset falls back to a plain `Unlit/Texture` placeholder, with a console
   warning); use `ParaViewLinkUnity/Materials/M_ScalarField.mat` or
   `M_ScalarFieldLit.mat`, or your own material using the same `_Colormap`
   texture parameter convention.
3. Add a `SimContainerObject` component to that **same** GameObject (it's
   read via `GetComponent`, not found elsewhere in the scene) — ideally a
   Cube, since its Transform (position/rotation/scale) *is* the container:
   position = world-space center, scale = world-space extents. This is
   Unity's equivalent of Unreal's Sim Container actor, read by
   `MeshReceiver.ComputeCoordTransform()`.
4. Leave `DataManagerHost` empty for auto-discovery, or set
   `DataManagerHost`/`DataManagerPort`/`DiscoveryPort` directly on the
   component in the Inspector.

## Ports

| Port | Direction | Purpose |
|---|---|---|
| `--listen-port` (9000) | ParaView → DataManager | ParaView's `PVLinkDomainBoundsFilter` dials this. |
| `--client-port` (9010) | Unity/Unreal → DataManager | Clients dial this to receive the relayed/replayed stream. |
| `--discovery-port` (9011) | DataManager → LAN (UDP broadcast) | Presence announcement; see Discovery. |

## Discovery

Every running DataManager broadcasts a small UDP text packet on the
discovery port roughly every 2 seconds — `PVLINK-DISCOVERY 1`, its
`client_port`, and whatever `project` it's currently serving (empty if live
mode hasn't seen a ParaView source yet). Each client listens for these and
keeps a live, pruned list of what's on the LAN (`DataManagerDiscovery` in
both engine plugins).

If a client's configured host is empty, it waits ~3 seconds for that list to
settle and auto-connects only if exactly one DataManager was found;
otherwise it stays idle and logs how many it saw. Either way,
`ConnectToDataManager(host, port)` can re-point a running client at a
specific DataManager at any time — there's no UI for this yet (a picker
built on `GetDiscoveredDataManagers()`/`DiscoveredDataManagers` is future
work), but the underlying capability is there.

## Backpressure

ParaView is never allowed to race ahead of what a client can actually
render: `MSG_TYPE_UPDATE` is the one message type that gets an ack, and
`PVLinkConnectionManager.mark_mesh_sent()` blocks the pipeline thread until
every connected client has acked. Multiple `PVLinkMeshSenderFilter`s in one
real pipeline cycle are batched into a single `UPDATE` via a ParaView view
`EndEvent` observer (installed once, lazily) rather than each sender firing
its own — see `mark_mesh_sent()`'s docstring in `PVLink.py` for the full
history of why (a debounce-timer approach starved under `AnimationScene`
`Play()`; a naive per-sender-synchronous approach worked but desynced
multi-mesh frames across UE ticks).

## Logging

All four components default to quiet, low-frequency logging (errors and
one-time state changes only) with chatty per-message logging gated behind
an opt-in flag: `PVLink.py`'s `PVLINK_VERBOSE` env var (or set live from the
Python shell), `datamanager.py`'s `--verbose`/`-v`, Unity's `MeshReceiver`
`VerboseLogging` Inspector toggle, and Unreal's `UE_LOG` `Verbose` level
(use the Output Log's own verbosity filter).
