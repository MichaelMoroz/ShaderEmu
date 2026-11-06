# How much Udon carries between clients

A probe for the display stream's question (`docs/world/share.md`): how many bytes a second, how late, by
which channel and over how many objects.

## Measured, 31 October 2026

Two desktop clients on one computer, one account, through VRChat's servers, 45 fps, noise payloads, on the probe's world
with SDK 3.10.5. Each figure is one slot: 6 s of arrivals of a 20 s slot, or 12 s of a 40 s slot (PlayerData, and the first
10,000 of events + manual sync), so read them to a few hundred bytes. The clients' logs are not kept (they name the account).

| Channel | Packet | Objects | Asked | Heard B/s | One-way ms, p50 / p95 / most | Ping round trip p95 |
|---|---|---|---|---|---|---|
| events + manual sync | 1,000 | 1 | 10,000 | 10,000 and 9,833 | 79 / 146 / 146 and 77 / 632 / 1,432 | 177, 182 |
| events + manual sync | 1,000 | 1 | 14,000 (gated, sent 10,250) | 9,167 | 78 / 967 / 990 | 733 |
| events | 1,000 | 1 | 10,000 | 7,167 | 1,024 / 2,769 / 3,058 | 2,406 |
| events | 1,000 | 4 | 10,000 | 7,333 | 926 / 2,558 / 3,058 | 2,513 |
| manual sync | 1,150 | 1 | 10,000 (sent 9,056) | 6,900 | 1,060 / 2,215 / 2,437 | 177 |
| manual sync | 1,000 | 4 | 10,000 | 7,000 | 1,184 / 2,844 / 3,240 | 177 |
| PlayerData | 1,000 | - | every frame | 1,833 | 73 / 114 / 140 | 178 |
| PlayerData | 8,000 | - | every frame | 13,333 | 983 / 1,873 / 1,874 | 178 |
| PlayerData | 64,000 | - | every frame | 10,667 | 18,576 / 22,938 / 22,939 | 177 |

- One channel holds about 7 KB/s however many objects carry it. What is asked beyond that waits, and the wait grows
  while it is asked for. `OnPostSerialization` and the send call do not say so: the sender's counts are not what arrives.
- Events and manual sync together carry about 10 KB/s, and more than that is not had by asking (14,000 gave 9,167 with `IsClogged` on 20% of frames).
- A backlog of events delays other events (the pings); a backlog of manual sync does not.
- With a 100-byte packet (136 characters), continuous sync's sender sent and the other heard nothing, in one 20 s slot. Smaller ones were not run.
- The editor says nothing of this: ClientSim has no limit on events (1.4 MB/s, looped back) and no `OnPostSerialization` (`editor_run.log`).

Not run: manual sync or events of other sizes and on 2 or 8 objects, scene objects, both clients sending, zeros, 3 players, two
computers or two accounts, a real display stream over two channels. `only` on the probe picks cases: ask before a long run.

It is a scene of its own (`Assets/NetProbe/NetProbe.unity`, a floor, a VRCWorld, the probe), with nothing of the
machine in it, so what bounds a number is the network and not the frame. Its code is not in the repository: it lies
in the Unity project's `Assets/NetProbe` alone (`NetProbe.cs`, `NetProbeLink.cs`, `NetProbeCont.cs`, each with an
`UdonSharpProgramAsset` beside it, and `Editor/NetProbeScene.cs`), and the report script below was not kept;
"ShaderEmu/Make the network probe's scene" replaces the open scene, so save or give up what that scene holds first.

## What it does

Each client takes the case from the instance's clock, a slot of `slotMs` (30 s) each, so nothing is sent to agree on it.
The lowest player number sends (both, in the cases that say so) for the first 3/4 of the slot; the rest drains. Bytes
heard count from 1/6 to 3/4 of the slot. A packet is noise (or zeros, to see whether anything compresses it) after a
header with the case, the link, the sender, a count and the instance's clock. Every client writes one `[NetProbe]` line a
slot to its log. Two pings a second by events of their own give a round trip that needs no agreement between
clocks, and show what a load does to other traffic.

The cases are the table in `NetProbe.Start`: manual sync of 200 to 64,000 bytes over one object, the same over 2, 4 and 8
of a player's objects and over scene objects; events of 200 to 16,000 bytes, over 1 to 8 objects and two names; events
and manual sync held to a rate from 6 to 20 KB/s whatever `IsClogged` says; both clients at once; zeros; continuous sync
(strings: it refuses arrays); `PlayerData.SetBytes`; events and manual together.

## Running it

- The editor: the hub's `solo` set, `slotMs` 12,000, `only` a few cases, play mode. ClientSim has no network (`docs/world/share.md`,
  "Testing"), so this says only what the calls accept and how often their callbacks come.
- Two real clients: "Build & Test" with two clients, or `VRChat.exe --no-vr --fps=45 --profile=N --url=create?...` with
  the same `--url` for both, to hold the frame rate and the load down. All 57 cases are 28.5 minutes.
- The figures were read from the clients' `[NetProbe]` lines in `%USERPROFILE%\AppData\LocalLow\VRChat\VRChat\output_log_*.txt`
  (by a `netprobe_report.py LOG_A LOG_B` of the time, not kept).
