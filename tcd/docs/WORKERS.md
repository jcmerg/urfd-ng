# tcd module workers

**English** | [Deutsch](WORKERS.de.md)

How tcd spreads the modules over the CPU cores, and what it does when a worker or an AMBE device falls behind. The setting itself, `Workers`, is explained in the [README](../README.md#module-workers).

## Contents

- [Workers and modules](#workers-and-modules)
- [A worker that falls behind](#a-worker-that-falls-behind)
- [AMBE devices](#ambe-devices)
- [Codec plugins](#codec-plugins)
- [The report to urfd](#the-report-to-urfd)

## Workers and modules

Every module belongs to one worker thread. The worker does all the work on its module's frames, one codec after the other: decoding, AGC, encoding to the other codecs in software, the AMBE devices' answers, and sending the frames back to urfd. Only the module's worker touches a frame, so the codecs of one frame need no locks between them; the workers of different modules run at the same time.

`Workers = auto` takes one worker per CPU core tcd may use: the cores of its affinity mask (`docker --cpuset-cpus`) and at most its cgroup's CPU quota rounded up (`docker --cpus`, systemd `CPUQuota=`), never more than one per module. The modules are dealt out in turn in the order of `Modules` and `StandbyModules`, so with 9 modules on 8 cores the first and the ninth share a worker. The log names the split at start: `8 module workers: FR S A C D H N P`.

The state that belongs to a stream (the codec plugins' states, the AGC, the b2 gain, the noise suppression, the stream log) is kept per module, each with its own lock, so the workers do not wait for each other. The AMBE devices have one lock of their own, taken only where a device comes in.

## A worker that falls behind

A worker can fall behind when a codec plugin hangs, when one module's sending blocks, or when the host is overloaded. A packet from urfd that waited longer than 400 ms for its worker is then dropped: that late it is of no use to urfd, and the worker catches up. Never dropped are:

- a stream's first frame: direct transcoding starts there only
- a stream's last frame: the codec states, the AMBE channel and the stream log end with it
- an AMBE device's answer: that frame is done

A worker's queue holds at most 2000 packets as a bound for the memory. The log says once a second that a worker drops frames. The web monitor and `tcdmon` show each worker's load (the share of a CPU core it worked in the last second) and the longest a frame waited for it; red from 70 % or 20 ms. Waiting for a codec plugin without parallel calls counts as work there.

M17 codes a payload in two packets, the second with the first's half. A second packet whose first did not come (dropped, or lost before urfd) is coded against silence, not against a half of an older payload.

## AMBE devices

The devices keep their own threads for the serial line: a feeder that sends the frames, a reader that takes the answers. A worker hands a frame to the device and goes on with other frames meanwhile; the answer goes back to the module's worker.

- **FIFO**: the feeder never sends a frame into a full chip FIFO (5 frames on a DV3003, 2 on a DV3000), as the chip would drop it and the next answer would go to the wrong frame.
- **Too late for the device**: a frame that waited a second to go to its device goes on with silence, so a device that is slower than the frames come (but still answers) does not queue them without end.
- **Lost answers**: an answer the device owes for 300 ms is taken as lost (see [Lost answers](../README.md#lost-answers-devicecompand)); its frame goes on with silence.
- **A hung or lost device**: a device that answers nothing for 1 s while frames wait, or that is unplugged, is taken out (see [A device lost while tcd runs](../README.md#a-device-lost-while-tcd-runs)). The frames it was sent and the ones still waiting go on with silence at once, as do frames handed to it while it is closed (phased out, reopened for another `DeviceCompand`).
- **Order**: an answer can come back after newer frames of its stream (silence for a frame the device did not code, a frame left on a lost device). urfd takes the frames of a stream by their count: an older one that comes late is dropped there.
- **Channel pool**: a stream's AMBE channel is freed only after its last frame is queued to the device, so another module's first frame cannot come before it.

## Codec plugins

tcd's own vocoders (OP25, Codec2) are called in parallel. A codec plugin is when it sets `TCD_PLUGIN_CONCURRENT` (codec API 5): then the workers call it at the same time on the states of different streams, `open` and `close` included, never on one state at once. A plugin without the flag (API 2 to 4) is called by one worker at a time; the log says so at start. See [Writing a plugin](../README.md#writing-a-plugin) and [`CodecPlugin.h`](../CodecPlugin.h).

## The report to urfd

After a transmission tcd sends urfd a report of its audio (levels, AGC gain, FEC). It goes out as soon as the stream's last frame is coded and sent back, whatever it waited for (a worker, a device, a lost answer), so every frame of the stream is in it. A stream whose last frame got lost is reported 2 s after its last activity.
