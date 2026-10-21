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

## What was there, and what was added

- The kernel the image is built from (pimaker's fork, `linux.config`) has had `CONFIG_NET`,
  `CONFIG_INET` (TCP, UDP, ICMP), Unix and packet sockets and the loopback device all along,
  and BusyBox has had `ping`, `nc`, `telnet`, `telnetd`, `wget`, `ifconfig`, `route` and
  `netstat` built in. Nothing of the stack was switched on for this; no IPv6, no netfilter.
- The fork's own network card (`rvcnet`: an Ethernet device behind CSRs 0x0c0 to 0x0c3, which
  this machine does not have) registered a dead interface at every boot. It is left out of the
  build now (`install_drivers.py`), and `linux/kernel/shaderemu_net.c` takes its place.
- `/dev/pts` is mounted by `emuinit` (telnetd opens its terminals by name), and `nc` and
  `telnetd` have names in `/usr/bin` (two-line scripts: the ROM's BusyBox links lack them).
  `ping` is upstream's, as it was.

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
| `0x870003e0` | guest | `sent`: packets the guest has put into the window so far |
| `0x870003e4` | guest | `taken`: packets the guest has read from the ring so far |
| `0x870003f0` | host | `acked`: the value of `sent` up to which the host has taken the window's packets |
| `0x870003f4` | host | `delivered`: packets the host has put into the ring so far |
| `0x870003f8` | host | the machine's number N (0: no link) |
| `0x870003fc` | host | 0 |
| `0x876b8000` | guest | the window: 640 bytes, packets for the others |
| `0x876b8400` | host | the ring: 8 places of 640 bytes, packets for this machine |

The two control texels (`0x3e` and `0x3f` of the control row) are among the first 64 that
every host reads back already. The window and the ring are one row of the state texture:
RAM texel `0x76b800`, row 3863 (64 + 3799), the window at x 0 to 39 and the ring at x 64 to
383. The console is x 40 to 57 (`docs/machine/console.md`). That row was the top of Nano-X's scratch textures (`DATA_END` in `scr_shaderemu.c`); it
lies in band 29, which a control pass that draws bands 28 and 29 covers.

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
  delivery. The ring's row is in band 29, which the control pass draws anyway.
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
- Until 21 October 2026 the packets rode in the share object (`EmuShare`) with the display, the
  input and some 240 bytes of other fields every sending, and waited for that object's last
  sending to go out and 50 ms after it. Measured that day between two real clients on one
  computer (the hub's `lanTest`, below), `ping -c 20` each way one after the other on an
  otherwise quiet link, round trip min / mean / max in ms, each client's:

  | Session | in `EmuShare` (before) | `EmuNet` serialized | `EmuNet` events |
  |---|---|---|---|
  | 1 | 216 / 302 / 417, 250 / 308 / 377 | | |
  | 2 | | 243 / 300 / 357, 239 / 282 / 395 | |
  | 3 (both in one) | | 250 / 307 / 362, 238 / 297 / 363 | 198 / 231 / 280, 199 / 229 / 271 |
  | 4 (both in one) | | 214 / 299 / 353, 192 / 330 / 822 | 201 / 226 / 278, 183 / 241 / 630 |

  With both machines pinging at once and a fetch beside (the test before it was staggered),
  the client that was not hit by the hold below had 279 / 505 / 1,158 and 235 / 394 / 887 in
  `EmuShare`, 267 / 364 / 1,053 and 239 / 318 / 604 in `EmuNet` serialized.

  A batch's time on the way (`sentMs` to its arrival) was 100 to 120 ms serialized, of which
  some 40 is the wait for VRChat's next sending (`OnPostSerialization`), and 50 to 90 ms as an
  event; the rest of a round trip, some 80 ms, is the two machines' readbacks, frames and ticks.
  Under a fetch of a picture (TCP, full packets) either way had batches 0.4 to 1.2 s on the way.
  Some 50 s after the clients start, VRChat once held everything 2 to 3 s (both clients at
  once, either carrier): a ping then has seconds-long round trips.
- A batch goes out whether or not anybody watches the display, and while packets flow the
  display goes with one sending in six only (below, "A game has most of the world's link").
- Checked in play mode with one visitor (before 21 October 2026, the packets then in `EmuShare`): `ifconfig lan0` says `10.0.0.1`; `ping -c 3 10.0.0.2`
  leaves three packets of 84 bytes in the machine's queue (`netSent`), acked, and three
  batches in the visitor's object; an echo request written into a spawned player's `net` (with
  `netSeq` raised and `_onDeserialization` sent) is taken by the guest (`taken` 1) and its
  36-byte reply comes out. `EmuMachine`'s `netSent`, `netReceived`, `netBad` and the hub's
  `netBatches`, `netTaken` are there to read.

## The guest's driver

`linux/kernel/shaderemu_net.c` polls the device from the kernel's tick (`shaderemu_net_tick`,
put into `update_process_times` by `tick_input_hook.py`, as the input's poll is), not from a
timer of its own: a timer that expired every tick cost a running program some 320 instructions
a tick in the timer wheel. An idle machine wakes at the next tick (`shaderemu_net_idle`, from
the idle loop) for five seconds after a packet went or came (`NET_AWAKE`), and after that polls
on the ticks it has anyway (the input's, every fourth tick on a machine nobody touches): the
first packet to a machine idle that long waits up to 40 ms of its time. A kernel without the
hook polls from the timer as before. Measured on 21 October 2026 on `rvc_cpu` with the link up
(`--net`, as every machine in the world has): an idle shell 229 thousand instructions a second
to 157 (2,184 passes in 20 s to 1,277), a shell loop 628.67 million instructions to 622.66
(-0.96%), the same count every run; ping and a file through `nc` on a clean link and on a bad
one (5% loss, 250 ms each way) as before, the sums equal.

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
  is still joining the first core makes the messages itself. On the serving machine the
  first core's frame was 134 thousand instructions before its wait for the display and is
  75 (the server's part 70 to 5; `QUAKE_SERVER=inline` is the old way). `QUAKE_NET=1` prints what packets cost, every 200 sent.
- **Doom**'s port had its network file all along (`i_net.c`, linuxdoom's: every player sends
  its moves to every other over UDP port 5029) and needs no change: `doom -net 1 .10.0.0.2`
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
  `nxnet --find`, `--ls ADDRESS PATH`, `--get ADDRESS PATH FILE`. Between two interpreters
  (`--net 47000 --net-id 1` and `2`): `--find` printed `10.0.0.1 Computer 1`, `--ls` its
  Home, and an 83 KB picture came in 4 s, the same bytes. `NXNET_SELF=1` asks this computer
  itself (the loopback), for a test with one machine. In the world's play mode with one
  visitor, typed through "Console paste": `ps` has one `nxnet --serve`, `NXNET_SELF=1 nxnet
  --find` prints `127.0.0.1 Computer 1`, `nxnet --ls 127.0.0.1 /` the Home's four folders,
  and a plain `nxnet --find` leaves three packets in the machine's `netSent` and finds nobody.
  Between two real clients (the hub's `lanTest`, a "Build & Test" with two clients on one
  computer, 12 October 2026): each machine's `ping -c 5` of the other, 5 received; then each
  one's `nxnet --find` printed the other (`10.0.0.1 Computer 1`, `10.0.0.2 Computer 2`), its
  Home listed four entries, and a picture of 14 KB fetched from it was the same bytes as the
  machine's own copy, some five seconds after the listing; 84 and 88 packets each way. The
  hub's `lanDoom` asks for the game of Doom instead, as the test was before. A link that carries 600 bytes a
  sending (the world's then; 1,000 now, below) is some 4 KB a second: a picture of 300 KB is over a minute.
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
  fifteen packets a second. Between two `rvc_cpu` machines (`--net 47300 --net-id 1` and
  `2`, `nxnet --serve` started by hand on the host, 14 October 2026): the join found
  `10.0.0.1 Computer 1 [quake3]`, the host printed `Client 0 connecting` and `UnnamedPlayer
  connected`, and both said the player entered the game. Not in the world, and not played. "Doom: network game"
  (`nxgame doom host`) is started on every computer that plays and Start pressed on each:
  the players are the computers that say `[doom]`, numbered in their addresses' order, and
  each runs `doom -net N` with the others' addresses. `nxgame doom now` asks nothing and
  waits eight seconds instead (`NXGAME_WAIT`), for a test. A game looks for the others with
  `nxnet --find 12`: a computer busy with a game, over the world's link, answers seconds
  late, and the default four are for the Network window. Checked on 12 October 2026 through
  these scripts (the hub's `lanTest` with `lanDoom` or `lanQuake`, two clients on one
  computer, the second of them at a fifth of the first's speed): Doom said `player 1 of 2 (2
  nodes)` and `player 2 of 2` and ran 12 to 25 tics in five seconds on both; Quake's host
  said `[quake]`, the other printed `joining Quake at 10.0.0.1` and `Connection accepted`,
  and in one run of two went on into the level (787 packets one way and 257 the other in
  the minute after), in the other neither sent a packet after the acceptance (not found
  why). Between two harness machines the same host and join went into the level both
  times, on the processor (6,600 packets each way in two minutes) and in the shader (1,312
  and 895). Tiberian Dawn and Red Alert are built without the game's network code
  (`-DNETWORKING=OFF`), and Tiberian Dawn's data is the demo's, which has no maps for
  several players: neither plays over the network.
- **A game has most of the world's link, and Quake keeps to it.** Quake over the world's link
  blinked its "nothing from the server" sign all the time: a move and an update a frame are
  more packets than a client's eight or so sendings a second carry, and the machine's queue
  of 32 overflowed (48 times in a minute). While a machine's packets flow, the hub now gives
  them up to 1,000 bytes of every sending (`NetBatchMost`; it was 600) and the display one
  sending in six (it was every other); the machine keeps 64 packets each way (`NetQueue`).
  Quake sends a move to a server on another machine, and an update to a client on one, eight
  times a second (`net_rate`; 0 for one a frame, as before), and shows the sign after a
  second and a half without a packet (it was 0.3 s). Two clients on one computer after
  this: 5.6 packets a second each way, what one sent the other received to within three in
  450, no queue overflow, no batch missed; the picture of it is the developer's to judge.
- Anything else with sockets: `busybox httpd -p 80 -h /usr/share` on one machine and `wget
  -O /tmp/w http://10.0.0.1/web-css.html` on another gives the same file. Nothing outside
  10.0.0.0/16 exists.

## Checking it

`--until` texts and two machines at once want a script; these are the commands it runs, each
machine with its own `--uart-log`. Start them from PowerShell.

- **Ping, both ways** (interpreters, 13 s): each of
  `bin\rvc_cpu.exe --quiet --net 47000 --net-id N --uart-log logs\lan_N.log --expect "/ # " --send "sleep 4; ping -c 4 10.0.0.M\n" --until "packet loss"`
  must say `4 received`. On the shader: `bin\rvc_harness.exe --rvc experiments\rvc_opt --image
  linux-net --load-state build\snapshots\rvc_shell.snap --no-stdin --net 47000 --net-id N
  --input "..."`.
- **A file through TCP on a bad link** (`--net-loss 5 --net-delay 250` on both, 60 s): on 1,
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
  level (40 s at full speed): its search lasts a second and a half. Two shader machines at
  once run at half speed each and need more than the five minutes a test may take here: the
  server on `rvc_cpu` and the client on `rvc_harness` take 135 s.
- **Doom** (two `rvc_harness`, 60 s; it needs the sound card, which `rvc_cpu` lacks): both
  must print `player N of 2 (2 nodes)` and then `doomstat:` lines with about 170 tics each
  (five seconds at 35 a second).
- **Two real clients**: set the hub's `lanTest` and the gate's `testOpen` (`EmuGate`: a test
  client never walks out of the corridor, so the world's parts come on 5 s after it arrives),
  the SDK's number of clients to 2, and "Build & Test". Both machines go on; 60 and 85 s after
  both are on, the lower and the higher player pings the other 20 times over serializations,
  then (from 110 s) both send batches as events and ping again at 115 and 140 s; then each finds
  the other with `nxnet`, lists its Home and fetches a picture. Each client's log has the
  `round-trip` lines and the `[Lan] times` lines. Turn both switches off and the clients back to
  1 before saving the scene. A scene from before 21 October 2026 gets the network's player
  object from "ShaderEmu/Add the network's player object to the open scene".
- The single-player sums of `docs/games/quake.md` are as they were. `QUAKE_SUM` leaves a player's
  name out now, as it left the world's model out: its place is counted from QuakeC's
  strings and moved when the network drivers put console commands of their own on the heap.
  With that, the build with `net_none` and this one print the same twelve sums on e1m5, and
  so do `inline`, `late` and `wait`.

## What it costs

Measured on 10 October 2026 (`rvc_cpu` for instructions; RTX 5090, D3D11 and fxc2 for times).

| | Before | After |
|---|---|---|
| Kernel `Image` | 4,770,320 bytes | 4,770,192 (the fork's card out, ours in) |
| Instructions to the prompt | 10,707,092 | 10,701,127 |
| An idle machine, no link (30 s from power-on, less the boot) | 205,500 a second | 203,300 |
| An idle machine with a link and nobody on it | | 216,900: the tick's poll is 13,600 a second, 136 a tick |

| | |
|---|---|
| `ping` between two interpreters | 20 to 40 ms |
| `ping` between two shader machines | 40 to 68 ms from a cold boot, 21 to 42 from the snapshot |
| 204,800 bytes through `nc`, a clean link, two shader machines | 7.6 s: 27 KB a second, 408 packets one way and 281 back |
| the same with 5% loss and 250 ms each way | 24.4 s (interpreters: 25.7 s): 8 KB a second; 27 of 394 and 15 of 300 packets lost, sums equal |
| Quake, a packet sent (`sendto`) | 4,650 instructions on a client, 5,800 on a server |
| Quake, a packet received (`recvfrom`) | 1,240 to 1,480; a read that finds nothing 540 |
| Quake, a client at 18 frames a second | 18 packets out, 14 in and 18 empty reads: 113,000 instructions a second, 4% of the machine |
| Doom, two players, two shader machines | 159 to 175 tics in five seconds (alone: 175); about 24 packets a second each way |

## What is not done, and what to know

- **In the world `ping` and Doom have been run between two real clients** (the hub's `lanTest`,
  a "Build & Test" with two clients on one computer, 10 October 2026): each machine's `ping -c
  5` of the other, 5 received; then `doom -net`, both printing `player N of 2 (2 nodes)`, some
  2,200 to 2,400 packets each way in two minutes, and 64 to 70 tics in five seconds on both
  once the game had settled (alone: 175; two clients share this computer's graphics card). No
  TCP and no second computer yet.
- **One window a readback's round trip.** Before the window took several packets, Doom ran
  at 2 tics a second between two shader machines: each packet waited for the last one's ack,
  some 60 ms, and Doom sends 35 a second. A host whose readbacks take longer gets
  proportionally less: 640 bytes a round trip is the ceiling.
- TCP's first timeout is the kernel's own second; the link's round trip is under it, so it
  was not raised. No congestion setting was changed.
- Whether two Quake players see each other was checked by the server's `status` and the
  packets, not by a picture. Nothing was played by hand, and connecting by address or from
  the menu was not tried.
- Doom on a link with VRChat's delay was not tried: its game waits for every player's moves.
- `rvc_cpu` has no sound card and Doom there ends in its music code (`snd_page` is null after
  "no sound card"), with or without a network: Doom's checks are the shader machine's.
- A machine's packets to `10.0.0.M` with M over 16, or to any other network, are dropped by
  the harness; in the world the number is the VRChat player's.
- Limited broadcast and the network's broadcast leave on `lan0`; Linux does not answer
  `ping` to either (its default).
- `linux/prebuilt` has had the network's kernel (`shaderemu_net`), `nc`, `telnetd`, `nxnet`
  and `nxgame` since it was saved again.

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
