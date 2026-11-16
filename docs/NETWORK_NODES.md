# Network nodes

A network node is a virtual node that is shared between SoundRoot instances on
the same local network. This document describes what it does, how it is built,
the wire protocol, and what it does and does not protect against.

Status: implemented in the plasmoid (`backend/net/`). Covered by
`tests/tst_netprotocol`, `tst_jitterbuffer`, `tst_netengine` and the network
cases of `tst_routingengine`.

## 1. What it does

Every virtual node has an **IN** side and an **OUT** side.

- A plain virtual node passes what is played into IN on to the outputs wired
  to OUT, on the same machine.
- A network node joins a **channel**. What is played into IN is sent to every
  other device on the channel. What those devices send comes out of OUT.
  A device never hears its own input on its own OUT.

```
        Desktop                                        Laptop
 ┌──────────────────────┐                      ┌──────────────────────┐
 │ Firefox ─► IN        │   encrypted UDP      │        OUT ─► Speakers│
 │        [node "den"]  │ ───────────────────► │  [node "den"]         │
 │ Headset ◄─ OUT       │ ◄─────────────────── │        IN ◄─ Spotify  │
 └──────────────────────┘                      └──────────────────────┘
```

A channel is a **name** and a **passphrase**. Devices whose node has the same
two values are on the same channel; nothing else has to be configured. More
than two devices can share a channel: each one hears the mix of all others.

A network node is wired like any other node, so audio can go from local
applications to remote devices, from remote devices to local outputs, or into
another virtual node.

It is only online while its room is switched on.

## 2. Using it

1. In a room, add a virtual node, open its settings (the sliders icon) and
   turn on **Share over the local network**.
2. Enter a channel name and a passphrase (at least 8 characters; the refresh
   button generates a strong one, the copy button copies it).
3. Do the same on the other device, with the same channel name and passphrase.
4. Wire applications into the node's IN to send, and the node's OUT to an
   output device to listen. Switch the room on.

The node's card shows who is connected.

### Firewall

Devices must be able to reach each other over UDP:

| Port | Purpose |
|---|---|
| 47780 | discovery beacons (multicast group 239.255.77.80) |
| 47781 | everything else; if it is taken, a free port is used instead |

Many distributions block both by default. With firewalld:

```bash
sudo firewall-cmd --permanent --add-port=47780-47781/udp
sudo firewall-cmd --reload
```

With ufw: `sudo ufw allow 47780:47781/udp`.

### When devices do not find each other

Discovery uses multicast, which some Wi-Fi access points and guest networks
do not forward. Enter the other device's address in the node's **Addresses**
field on *one* of the two devices (`192.168.1.20`, or `192.168.1.20:47781`).
The other side learns the address from the first packet it receives.

Check, in this order: both rooms are switched on; channel name and passphrase
are identical (the name is case-sensitive); the firewall allows the two ports;
both devices are on the same network.

## 3. Why it is built this way

| Question | Decision | Reason |
|---|---|---|
| Transport | Own protocol over UDP | PipeWire's RTP modules are unauthenticated and unencrypted; the ROC modules are not built on every distribution (not on the one this was developed on). A small protocol works the same on PipeWire and PulseAudio. |
| Topology | Each device sends directly to each other device | No server to run or to fail. Unicast also avoids multicast audio, which Wi-Fi delivers poorly. |
| Discovery | Authenticated multicast beacons, plus optional addresses | Needs no extra daemon (mDNS would need Avahi running). Beacons are encrypted, so they reveal nothing but an opaque channel tag. |
| Security | Shared passphrase → key; every packet encrypted and authenticated | One secret to share, no pairing dialogue, and nothing useful to anyone on the LAN without it. |
| Codec | PCM, 16 bit, 48 kHz, stereo | 1.5 Mbit/s is nothing on a LAN; no codec delay, no extra dependency. The header carries a codec field so Opus can be added without breaking old versions. |
| Where it runs | Inside the plasmoid, on its own thread | Simple to install and to keep in step with the UI. The network code (`backend/net/`) has no dependency on Plasma and can be moved into a separate service later. |

## 4. Architecture

```
                         RoutingEngine  (decides, per reconcile pass)
                          │        │
              startNode() │        │ openNetworkCapture / openNetworkPlayback
                          ▼        ▼
   ┌─────────────── NetEngine ─────────────┐      ┌──── PulseAudioBackend ────┐
   │ worker thread: sockets, timers, peers │      │ record stream of the      │
   │                                       │◄─────│ node's null-sink monitor  │
   │  capture queue ─► packets ─► UDP      │ PCM  │                           │
   │  UDP ─► jitter buffer per sender      │─────►│ playback stream, moved to │
   └───────────────────────────────────────┘ PCM  │ the wired outputs         │
                                                   └───────────────────────────┘
```

- **Sending.** The node is a `module-null-sink`. A record stream on its
  monitor delivers what applications play into it; `NetEngine` cuts that into
  5 ms packets and sends them to every peer that listens. During digital
  silence it stops sending after 200 ms.
- **Receiving.** Packets go into one jitter buffer per sender. A playback
  stream asks for audio; `NetEngine` mixes the buffers. The routing engine
  moves that stream to the outputs wired to OUT exactly as it moves
  application streams (a combine-sink for several outputs).
- **Threads.** Sockets and timers live on a worker thread. The audio thread
  only touches short, locked queues; the UI thread only sends commands.
  Neither ever waits on the network.
- **The record stream never follows another source.** It is opened with
  `PA_STREAM_DONT_MOVE` and its source is checked on every callback: if the
  node's sink disappears the stream ends, instead of being re-attached to the
  default source (the microphone).

### Jitter buffer

Target delay 60 ms. It places packets by timestamp (order does not matter, a
lost packet is a gap of silence), refills to the target after running dry,
skips ahead when a burst leaves too much queued, and drops or repeats single
frames to follow the drift between the two devices' sound cards.

By design the end-to-end delay is about 100 ms (10 ms capture, 60 ms jitter
buffer, 30 ms playback buffer); it has not been measured on real Wi-Fi. That
is fine for music and speech between rooms, and noticeable when the same sound
is also heard directly.

## 5. Protocol (version 1)

All integers are big-endian. Audio samples are little-endian.

### Keys

```
salt       = BLAKE2b-128( "soundroot-net-v1" || 0x00 || channel )
master     = Argon2id( passphrase, salt, ops = 2, memory = 64 MiB )   → 32 bytes
aead key   = crypto_kdf( master, id 1, context "srnaead1" )           → 32 bytes
channel id = first 8 bytes of crypto_kdf( master, id 2, "srnchan1" )
```

Channel and passphrase are trimmed and NFC-normalised UTF-8. All primitives
are libsodium's.

### Packet

```
 0  "SRN1"            magic and protocol version
 4  type (1)          1 Beacon, 2 Audio, 3 Bye, 4 Hello, 5 HelloAck
 5  flags (1)         0
 6  reserved (2)      0
 8  channel id (8)
16  sender id (8)     random, new every time a node starts
24  counter (8)       increases with every packet of this sender
32  ciphertext, then a 16-byte tag
```

The payload is sealed with XChaCha20-Poly1305. The nonce is
`sender id || counter || 8 zero bytes`; the 32-byte header is the associated
data, so it cannot be altered either.

| Type | Payload |
|---|---|
| Beacon | JSON: `{"v":1,"name":"<device>","node":"<node name>","listen":true}`. Unknown keys are ignored. Sent once a second: to the multicast group, to configured addresses and to known peers. |
| Hello | 8 random bytes (a challenge). |
| HelloAck | The 8 bytes of the Hello it answers. |
| Audio | `codec (1) = 0`, `channels (1)`, `reserved (2)`, `sample rate (4)`, `timestamp (8)` = sender's frame counter of the first frame, then the samples. 240 frames (5 ms) per packet: 1024 bytes in all, below the MTU, never fragmented. |
| Bye | empty. The sender leaves. |

### Joining

A device that receives an authentic packet from an unknown sender — or from a
known sender at a new address — sends it a Hello. The peer is **confirmed**
when the matching HelloAck arrives from that address. Only confirmed peers are
sent audio, and only their audio is played. A peer that is silent for 5 s is
dropped.

Receivers reject repeated counters (a 64-packet window, as in IPsec/DTLS).

### Compatibility

A device ignores packet types and codecs it does not know. A change that old
versions cannot ignore gets a new magic (`SRN2`).

## 6. Security

**Protected**

- *Confidentiality.* Audio, device names and node names are encrypted. Without
  the passphrase an observer sees only that some device uses some channel.
- *Integrity.* Nobody without the passphrase can inject or alter audio.
- *Replay.* Recorded packets cannot be played back later: within a session the
  counter window rejects them; a recording of an older session cannot answer
  the Hello challenge, so it never becomes a confirmed peer.
- *Scope.* Packets from addresses outside private/link-local ranges are
  dropped. Beacons are sent with TTL 1. Nothing is sent while no room with a
  network node is switched on.

**Not protected**

- Everyone who has the passphrase is trusted equally: they can listen and play
  on the channel. There are no per-device identities and no way to remove one
  device other than changing the passphrase.
- A weak passphrase can be guessed offline from captured packets. Argon2id
  makes each guess expensive, not impossible. Use the generator.
- Availability. Someone on the same network can still flood or jam it.
- The passphrase is stored in `~/.config/audiorouter/groups.json`, readable
  only by the user (mode 0600) but not encrypted.

## 7. Limits

- IPv4 only.
- Devices play in step to within the jitter-buffer delay, not sample-accurate:
  not suitable for stereo pairs made of two devices.
- Uncompressed audio: about 1.7 Mbit/s to each listening peer.
- No mobile app. The protocol above is everything one would need to implement.

## 8. Tests

```bash
cmake -B build && cmake --build build
ctest --test-dir build
```

`tst_netengine` runs two or three engines in one process over the loopback
interface: discovery by address, handshake, audio in both directions sample
for sample, mixing of several senders, wrong passphrase, forged and replayed
packets, leaving and timing out.

Its multicast case skips when the machine does not deliver multicast to itself
(a host firewall). To run it regardless, in a private network namespace:

```bash
tests/run-in-netns.sh build/tests/tst_netengine
```
