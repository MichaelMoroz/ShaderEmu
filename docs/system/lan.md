# The network: TCP/IP between machines

Every machine is a host on one network, with Linux's own stack: a program that uses sockets
works, and no game has a driver of its own. The machines' link carries bare IP packets. Each
host (the harness, the interpreter, the VRChat world) moves packets between its machine's
memory and the other hosts; nothing else about it is the host's business.

    / # ping 10.0.0.2
    / # telnetd -l /bin/sh                         (on one)      telnet 10.0.0.1     (on another)
    / # nc -l -p 5000 < /dev/null > /tmp/f         (on one)      nc -w 3 10.0.0.1 5000 < FILE
    / # quake -listen 4 +map e1m1                  (on one)      quake +connect 10.0.0.1
    / # doom -net 1 .10.0.0.2                      (on one)      doom -net 2 .10.0.0.1

## What the stack is

- The kernel the image is built from (`linux.config`) has `CONFIG_NET`, `CONFIG_INET` (TCP, UDP,
  ICMP), Unix and packet sockets and the loopback device; no IPv6, no netfilter. BusyBox has `ping`,
  `nc`, `telnet`, `telnetd`, `wget`, `ifconfig`, `route` and `netstat` built in.
- The fork's own network card (`rvcnet`: CSRs this machine does not have) is left out of the
  build (`install_drivers.py`), and `linux/kernel/shaderemu_net.c` takes its place.
- `/dev/pts` is mounted by `emuinit` (telnetd opens its terminals by name).

## Addresses

- The host gives its machine a number N (a control word, below). The machine's address is
  `10.0.H.L` with H = N >> 8 and L = N & 255, on the network `10.0.0.0/16`, whose broadcast
  address `10.0.255.255` is everybody. 0 means no link: the interface is down.
- Nothing is asked for and nothing is configured in the guest. The driver watches the number
  (four times a second while down) and sets the address, the mask and the interface's state
  itself when it changes; so a machine resumed from a snapshot gets the address of the host
  it wakes up in. It also brings `lo` up (127.0.0.1) and adds a route for `255.255.255.255`
  through the link, where programs that look for others on a "LAN" send.
- The interface is `lan0`: no hardware address, no ARP (`ARPHRD_NONE`, `IFF_NOARP`), an MTU
  of 576. A packet's destination address says whose it is. There are no names: `/etc/hosts`
  is not touched, and no machine routes for another.

## The device

All words are little-endian 32-bit words of RAM. A packet is an IP packet as it would be on a
wire, from the first byte of its IP header, byte i at the address of its first byte plus i;
20 to 576 bytes.

| Address | Written by | Contents |
|---|---|---|
| `0x810003e0` | guest | `sent`: packets the guest has put into the window so far |
| `0x810003e4` | guest | `taken`: packets the guest has read from the ring so far |
| `0x810003f0` | host | `acked`: the value of `sent` up to which the host has taken the window's packets |
| `0x810003f4` | host | `delivered`: packets the host has put into the ring so far |
| `0x810003f8` | host | the machine's number N (0: no link) |
| `0x810003fc` | host | 0 |
| `0x816b8000` | guest | the window: 640 bytes, packets for the others |
| `0x816b8400` | host | the ring: 8 places of 640 bytes, packets for this machine |

The two control texels (`0x3e` and `0x3f` of the control row) are among the first 64 that
every host reads back already. The window and the ring are one row of the state texture:
RAM texel `0x16b800`, row 791 (64 + 727), the window at x 0 to 39 and the ring at x 64 to
383. The console is x 40 to 57 (`docs/machine/console.md`). Nano-X's scratch textures end below that
row (`DATA_END` in `scr_shaderemu.c`); it lies in band 5, which a control pass that draws bands 4 and 5 covers.

A packet's place, in the window or in the ring, is: a word with its length in bytes, a word
with its number, two words that are not used, then its bytes. Packets are numbered from 1 in
each direction; packet k of a direction is the one that made the count k.

### From the guest: the window

- The guest writes packets one after the other from the window's start, each place beginning
  on a multiple of 16 bytes (the next begins 16 + the length rounded up to 16 after the
  last). After writing a packet's bytes and its two words it sets `sent` to its number.
- When `acked` equals `sent` the host has everything: the next packet goes to the window's
  start again. While the next packet does not fit after the last, the guest sends nothing
  (its queue holds 32 packets) until `acked` equals `sent`.
- The host, at every readback: if `sent` differs from what it last acked, it walks the window
  from its start, place by place, until the place whose number is `sent`. Places with numbers
  after the last one it took are packets to send on, in order. It then acks: `acked` = `sent`.
  The walk needs the control words and the window of one and the same pass.
- A walk that meets a length under 20 or over 576, or the window's end, before the place
  numbered `sent` takes nothing and acks nothing (the harness gives up after 200 such
  readbacks in a row and acks, so that a guest can never wait for ever).
- A host that starts on a machine that is already running (a snapshot) takes `sent` as it
  finds it at its first readback and sends nothing from before.
- A readback that never comes, or comes late, loses nothing: the guest waits for the ack.
  One window is 640 bytes for every round trip of a readback, so the link carries one full
  packet, or up to eight small ones (a game's are 40 to 80 bytes), each round trip.

### To the guest: the ring

- The host puts packet k (k from 1) into place (k - 1) mod 8, whole: its length, its number
  k, two zero words and its bytes; then `delivered` is k. Both arrive in one control pass.
- The guest reads the places from `taken` up to `delivered` in the kernel's tick and sets
  `taken`. A place whose number is not the one expected, or whose length is under 20 or over
  576, is counted as an error and skipped.
- The host puts in no more than fit: `delivered` - `taken` (as last read back, which may be
  a few passes old: that only makes the host careful) never exceeds 8. What does not fit
  waits in the host's own queue (the harness keeps 64 and drops the oldest).
- When `delivered` - `taken` is more than 8 as unsigned numbers, the two are not of one run
  (a host that started again counts from 0): the guest sets `taken` to `delivered` and reads
  nothing; the host puts nothing in until that has happened.

### What a host does every pass

In the shader the device is `GPU_NET` in `experiments/rvc_opt/src/gpu.h` (`gpu_control`): the
host texel is written from four uniforms and the ring from a texture, in the control pass.
Only a host that defines `GPU_NET` and has these compiles it (`gpu.shader` does):

| Uniform | |
|---|---|
| `_NetId` | the machine's number |
| `_NetTxAck` | `acked` |
| `_NetRxSeq` | packets delivered before this pass |
| `_NetRxCount` | packets that arrive in this pass, 0 to 4; `delivered` becomes `_NetRxSeq` + `_NetRxCount` |
| `_NetData` | a texture 160 x 4, RGBA8, not sRGB, read with `Load`: row e is the place of packet `_NetRxSeq` + e + 1, texel x its bytes 4x to 4x + 3 (red first) |

1. Read back the control row's texels `0x3e` and `0x3f` and the window's 40 texels (160
   words) with the rest. In the harness `popRow()` returns the window after the 256 control
   texels (D3D11).
2. Take the guest's packets from the window as above; hand each to the link.
3. Take what the link has for this machine: a packet whose destination is the machine's own
   address, `10.0.255.255` or `255.255.255.255`. Everything else is not for it.
4. Set the uniforms; with packets to deliver, fill `_NetData` and make sure the control pass
   draws the ring's texels in that pass (the harness draws a zone of 320 x 1 texels there).
   A pass drawn twice with the same uniforms writes the same things: nothing is delivered
   twice. The next pass has `_NetRxSeq` moved on and `_NetRxCount` 0.

`rvc_cpu` does the same on the interpreter's own memory (`net_pass`), with the same link
code (`harness/apps/rvc_net.cpp`). `rvc_harness --cpu` goes through the shader as any other
harness run, and copies the ring back into the interpreter's RAM after a pass that delivered.

## The harness's link

    bin\rvc_harness.exe ... --net 47000 --net-id 1
    bin\rvc_cpu.exe     ... --net 47000 --net-id 2 --net-loss 5 --net-delay 250

- Machine N listens on UDP port PORT + N of 127.0.0.1, N from 1 to 16. A packet for
  `10.0.0.M` is sent to port PORT + M as it is (one datagram, the IP packet); a packet for
  everybody to every port from PORT + 1 to PORT + 16 but its own. Packets for other
  addresses go nowhere. Machines of either program talk to each other.
- `--net-loss P` loses P percent of the packets a machine sends and `--net-delay MS` holds
  the others back MS milliseconds: the VRChat link's part. Both ends given 250 make a round
  trip of half a second.
- `RVC_NET_LOG=1` in the environment prints a line for every packet in and out.
- With `--net`, `rvc_cpu` keeps its guest clock to this computer's (it sleeps when the guest
  is ahead): machines that talk share one clock. Its instruction counts are as ever.
- The D3D12 backend has no network (`--net` with `--dxc` says so).

## In the VRChat world

The world is a host like the others (`EmuMachine.cs`, "the network"; `Machine.shader`'s control
pass defines `GPU_NET`).

- A machine's number is its visitor's VRChat player number, so its address is `10.0.H.L` of
  that: the first visitor of an instance is `10.0.0.1`. A machine that is off has no link.
- The readback's row is 1,032 words: 130 texels of the network's row (the window's 160 words,
  what the cores ran and which of them are asleep, then the console's 18) after state row 0 and the control texels (`Readback.shader`). `NetWindow` walks it at every round's row and acks; `NetFrame` puts up
  to four waiting packets into `_NetData` before a frame's rounds, which all draw the same
  delivery. The ring's row is in band 5, which the control pass draws anyway.
- Between visitors a packet travels through its sender's own player object for the network
  (`EmuNet`, the template "Net player"): the packets taken since the last batch, each after two
  bytes of its length, up to 1,000 bytes a batch (`NetBatchMost`) (one full packet, or several
  small ones), a count (`seq`) that tells a new batch from one seen, and the instance's clock
  when it was sent (`sentMs`). A batch is a network event, `EmuNet.Packets` (`[NetworkCallable(100)]`,
  to the others), sent the frame there are packets, at most every 10.5 ms and not while VRChat
  says the link is clogged (`SendNet`, in `Update` and again in `LateUpdate` for a readback that
  came in between). With the hub's `netEvents` off the same fields are serialized instead (manual
  sync, one batch in flight). Every other client's hub hands each packet to its own machine,
  which keeps it if its address is the machine's or everybody's. Nothing is repeated: TCP
  repeats what it loses, and the games' UDP expects loss.
- A batch rides in `EmuNet` and not in the share object (`EmuShare`) with the display: its sendings
  would make a packet wait for that object's other fields. The hub's `lanTest` and the `[Lan] times`
  lines measure a round trip and a batch's time on the way; a batch as an event is quicker than a
  serialized one, which waits for VRChat's next sending.
- A batch goes out whether or not anybody watches the display, and while packets flow the
  display goes with one sending in six only (below, "A game has most of the world's link").
- In play mode with one visitor: `ifconfig lan0` says `10.0.0.1`; `ping -c 3 10.0.0.2` leaves three
  packets of 84 bytes in the machine's queue (`netSent`), acked. `EmuMachine`'s `netSent`,
  `netReceived`, `netBad` and the hub's `netBatches`, `netTaken` are there to read.

## The guest's driver

`linux/kernel/shaderemu_net.c` polls the device from the kernel's tick (`shaderemu_net_tick`,
put into `update_process_times` by `tick_input_hook.py`, as the input's poll is), not from a
timer of its own, which would cost a running program instructions every tick in the timer wheel.
An idle machine wakes at the next tick (`shaderemu_net_idle`, from
the idle loop) for five seconds after a packet went or came (`NET_AWAKE`), and after that polls
on the ticks it has anyway (the input's, every fourth tick on a machine nobody touches): the
first packet to a machine idle that long waits up to 40 ms of its time. A kernel without the
hook polls from the timer.

## Programs

- **Quake** is built with its own `net_dgrm` and `net_udp` in place of `net_none`
  (`linux/quake/build.sh`). `UDP_Init` asks the interface for its address and its broadcast
  address instead of looking its own name up (`quake.patch`); without a link the address is
  the loopback's. `quake -listen 4 +map e1m1` is a server for four; `quake +connect NAME`
  finds it by a broadcast to port 26000 and its `hostname` (its address unless set);
  `+connect 10.0.0.1` is the game's way by address. A game
  with more than one player has its server's frame on a worker core as a game for one has
  (`server_shaderemu.c`): what the worker would send is kept, a reliable message to each
  connection at most, and goes from the first core when the frame is taken; while a player
  is still joining the first core makes the messages itself. `QUAKE_SERVER=inline` keeps the
  server on the first core. `QUAKE_NET=1` prints what packets cost, every 200 sent.
- **Doom**'s port has its network file (`i_net.c`, linuxdoom's: every player sends
  its moves to every other over UDP port 5029) : `doom -net 1 .10.0.0.2`
  on one machine and `doom -net 2 .10.0.0.1` on the other (the player's number, then the
  others' addresses, each after a dot). Both wait until all are there.
- **The Network program** (`linux/apps/nxnet.c`; Utilities in the Start menu, an icon on the
  desktop, the last place in Files). `nxnet --serve`, which `nx` starts with the desktop,
  answers the others: a UDP packet `NXNET?` to port 7474 (sent to `255.255.255.255`, which
  is routed through the link) is answered with `NXNET! NAME`, the name a line in
  `/tmp/nxname` or "Computer N"; over TCP at the same port `L /PATH` is answered with `OK`
  and a line an entry (`d` or `f`, a tab, the size, a tab, the name) and `G /PATH` with
  `OK SIZE` and the file. The paths are inside `/root`; a path with `/.` in it is refused,
  and nothing can be written. `/tmp/nxshare.off` (the window's "Stop sharing") makes the
  computer answer nobody. The window lists the computers that answered within a second and
  a half (F5 or the first key asks again), then the folder of the one chosen; a file is
  fetched to `/tmp/net/ADDRESS/` and given to `open`, or copied into `/root` under a name
  that is free ("Copy to my Home"); Escape gives a file up while it comes. From a script:
  `nxnet --find`, `--ls ADDRESS PATH`, `--get ADDRESS PATH FILE`, between two interpreters (`--net 47000 --net-id 1`
  and `2`) too. `NXNET_SELF=1` asks this computer itself (the loopback), for a test with one machine.
  Between two real clients: the hub's `lanTest`, a "Build & Test" with two clients on one computer;
  its `lanDoom` asks for the game of Doom instead. The world's link is some 4 KB a second at a thousand
  bytes a sending: a large picture is minutes.
- **Games over the network** (`linux/apps/nxgame`; the Start menu's Games has a folder
  Network, and the desktop its shortcuts). A computer that hosts a game says so to whoever
  asks who is there: the word in `/tmp/nxhosting` follows its name in the answer (`NXNET!
  Computer 1 [quake]`), whether or not it shares its Home. "Quake: host a game" (`nxgame
  quake host`) is `quake -listen 4 +map e1m1`, said to be hosted forty seconds after it
  starts (a join before the level is up finds no game); "Quake: join a game" connects to
  the first computer that says `[quake]`, or says that nobody hosts. "Quake III: host a
  game" and "Quake III: join a game" (`nxgame quake3 host`, `join`) are the same with
  `[quake3]`: the host is `quake3 +map q3dm17` for four, said to be hosted a minute after it
  starts, with a client on the LAN kept to 3,000 bytes a second as any other
  (`sv_lanForceRate 0`, `sv_maxRate`), and the join asks for that rate, ten snapshots and
  fifteen packets a second; not tried in the world, and not played. "Doom: network game"
  (`nxgame doom host`) is started on every computer that plays and Start pressed on each:
  the players are the computers that say `[doom]`, numbered in their addresses' order, and
  each runs `doom -net N` with the others' addresses. `nxgame doom now` asks nothing and
  waits eight seconds instead (`NXGAME_WAIT`), for a test. A game looks for the others with
  `nxnet --find 12`: a computer busy with a game, over the world's link, answers seconds
  late, and the default four are for the Network window. The hub's `lanTest` with `lanDoom` or
  `lanQuake` runs these scripts between two clients on one computer. Tiberian Dawn and Red Alert are built without the game's network code
  (`-DNETWORKING=OFF`), and Tiberian Dawn's data is the demo's, which has no maps for
  several players: neither plays over the network.
- **A game has most of the world's link, and Quake keeps to it.** A move and an update a frame are
  more packets than a client's sendings carry. While a machine's packets flow, the hub gives them up
  to 1,000 bytes of every sending (`NetBatchMost`) and the display one sending in six; the machine
  keeps 64 packets each way (`NetQueue`). Quake sends a move to a server on another machine, and an
  update to a client on one, eight times a second (`net_rate`; 0 for one a frame), and shows the
  "nothing from the server" sign after a second and a half without a packet.
- Anything else with sockets: `busybox httpd -p 80 -h /usr/share` on one machine and `wget
  -O /tmp/w http://10.0.0.1/web-css.html` on another gives the same file. Nothing outside
  10.0.0.0/16 exists.

## Checking it

`--until` texts and two machines at once want a script; these are the commands it runs, each
machine with its own `--uart-log`. Start them from PowerShell.

- **Ping, both ways** (interpreters): each of
  `bin\rvc_cpu.exe --quiet --net 47000 --net-id N --uart-log logs\lan_N.log --expect "/ # " --send "sleep 4; ping -c 4 10.0.0.M\n" --until "packet loss"`
  must say `4 received`. On the shader: `bin\rvc_harness.exe --rvc experiments\rvc_opt --load-state build\snapshots\rvc_shell.snap --no-stdin --net 47000 --net-id N
  --input "..."`.
- **A file through TCP on a bad link** (`--net-loss 5 --net-delay 250` on both): on 1,
  `nc -l -p 5000 < /dev/null > /tmp/f; md5sum /tmp/f`; on 2, `dd if=/bin/busybox of=/tmp/s
  bs=1024 count=200; md5sum /tmp/s; sleep 3; nc -w 3 10.0.0.1 5000 < /tmp/s`. The sums must
  be equal. (`nc -l` without `< /dev/null` waits for the console's end, not the
  connection's.)
- **A shell on the other machine**: on 1, `telnetd -l /bin/sh`; on 2, `(sleep 3; echo
  "ifconfig lan0 | grep Bcast; exit"; sleep 5) | telnet 10.0.0.1` must print `inet
  addr:10.0.0.1`.
- **Quake**: on 1, `nano-X -p & sleep 3; QUAKE_NET=1 quake -listen 4 +hostname first +map
  e1m1`, and `status` typed when it says `two entered the game` must list two players, the
  second at `10.0.0.2`; on 2, `nano-X -p & sleep 50; QUAKE_NET=1 quake +name two +connect
  first` must say `Connection accepted`. The client must not start before the server has its
  level: its search lasts a second and a half. Two shader machines at once run at half speed each: put
  the server on `rvc_cpu` and the client on `rvc_harness`.
- **Doom** (two `rvc_harness`; it wants the sound card): both must print `player N of 2 (2 nodes)` and
  then `doomstat:` lines.
- **Two real clients**: set the hub's `lanTest` and the gate's `testOpen` (`EmuGate`: a test
  client never walks out of the corridor, so the world's parts come on 5 s after it arrives),
  the SDK's number of clients to 2, and "Build & Test". Both machines go on; 60 and 85 s after
  both are on, the lower and the higher player pings the other 20 times over serializations,
  then (from 110 s) both send batches as events and ping again at 115 and 140 s; then each finds
  the other with `nxnet`, lists its Home and fetches a picture. Each client's log has the
  `round-trip` lines and the `[Lan] times` lines. Turn both switches off and the clients back to
  1 before saving the scene. "ShaderEmu/Add the network's player object to the open scene" adds what
  a scene lacks.
- `QUAKE_SUM` leaves a player's name out, as it leaves the world's model out: its place is counted from
  QuakeC's strings, which the network drivers' console commands move.

## What is not done, and what to know

- **One window a readback's round trip.** A host whose readbacks take longer gets proportionally less:
  640 bytes a round trip is the ceiling. In a world two clients on one computer also share one card.
- TCP's first timeout is the kernel's own second; the link's round trip is under it. No congestion
  setting is changed.
- Whether two Quake players see each other is checked by the server's `status` and the packets, not by
  a picture. Nothing has been played by hand, and connecting from the menu is not tried.
- Doom on a link with VRChat's delay is not tried: its game waits for every player's moves.
- `rvc_cpu` has no sound card without `RVC_SOUND=1`.
- A machine's packets to `10.0.0.M` with M over 16, or to any other network, are dropped by
  the harness; in the world the number is the VRChat player's.
- Limited broadcast and the network's broadcast leave on `lan0`; Linux does not answer
  `ping` to either (its default).

## The panel on the wall

On the right wall, between the danger zone and the rack ("Network panel", `EmuLan`, `ShaderEmuLan.cs`):
this visitor's own machine's address and counts (what `[Lan]` logs every five seconds), and a
line for every visitor's machine: address, whose, on or off, and how many sendings (batches
of packets, `netSeq`) it has made. Nothing is synced for it.

In the classroom every tower's network socket has a lamp, and every port of the three hubs
one: lit while the place's machine is on, flickering in a half second in which its owner's
`netSeq` moved (their own machine's: `netSent` and `netReceived`). `EmuStations.Lamps`,
`Blink.shader`.

## What the world writes to VRChat's log

`[Lan]` lines (`EmuMachine`, `EmuShareHub`), always on, in each client's own
`output_log_*.txt` (`%USERPROFILE%\AppData\LocalLow\VRChat\VRChat`):

- `this machine is on: 10.0.0.N (player N of M here)` at power-on, and `this machine is off`.
- The first 40 packets after power-on, one a line: `the guest sent ...` and `heard ...`, with
  its length, what it is (ICMP ping or answer, UDP, TCP with SYN, SYN+ACK, RST or FIN), its
  addresses and ports, and for one heard what became of it: `for this machine`, `not for
  this machine`, or `dropped, this machine is off`.
- The first 40 batches, each way: `batch K out to the others` and `batch K in from player P:
  N bytes, T ms on the way` (the instance's clock at arrival less `sentMs`), and `player P: N
  batches never seen before batch K` when a sender's count skipped (a batch is whatever its
  sender put last: one that came and went between two looks is lost).
- `[Lan] times:` every five seconds in which batches moved (`EmuShareHub.LanTimes`): how many
  went out (`as events`, or serialized with the mean time from asking VRChat to send one to its
  `OnPostSerialization`); how many came in and their mean and longest time on the way.
- Every five seconds in which something happened, the counts since power-on: sent by the
  guest; heard for this machine, for others, while off; given to the guest and taken by it;
  waiting to go out and in; dropped for a full queue.

To find why two visitors do not reach each other, read both logs: a packet one guest sent
must be a batch out there, a batch in here, `for this machine`, given and taken.
