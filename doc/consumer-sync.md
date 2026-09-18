# Experimental consumer-coupled DMABUF queues

This mode lets a producer share externally allocated dma-bufs with one
streaming CAPTURE owner. OUTPUT DQBUF completes only after that owner has
dequeued the frame and returned its slot with CAPTURE QBUF. The default
MMAP/write and DMABUF injection behavior is unchanged.

The interface is experimental and extends the DMABUF work in
[PR #665](https://github.com/v4l2loopback/v4l2loopback/pull/665). It is not an
upstream V4L2 ABI. The producer uses the two private setup ioctls from
`v4l2loopback.h`; consumers use standard V4L2 queue operations and EXPBUF.

A forwarding CAPTURE client can query `V4L2LOOPBACK_GET_CONSUMER_SYNC`
(`__u32`) to distinguish OFF, WAIT, and TRY. This read-only query works on
both endpoints. Hold CAPTURE REQBUFS ownership before relying on the answer;
an earlier answer is only a snapshot. Forwarders must reject OFF and older
modules without the query, because EXPBUF alone does not ensure delayed reuse.
The query does not establish CPU/GPU access protection or GPU completion.

## Setup and stable storage

On a node without a CAPTURE owner:

1. Open the OUTPUT fd and set its single-planar format with S_FMT.
2. Pass `__u32 mode = V4L2LOOPBACK_CONSUMER_SYNC_TRY` to
   `V4L2LOOPBACK_SET_CONSUMER_SYNC`. This lets a persistent producer skip
   readers that have not connected or cannot accept that slot.
3. Request the full pool with OUTPUT REQBUFS(DMABUF).
4. For every slot, call `V4L2LOOPBACK_BIND_DMABUF` with its index and fd.
   Both reserved words must be zero. Each dma-buf must cover QUERYBUF's
   buffer length, including page alignment.
5. Start OUTPUT streaming. All slots must be bound. STREAMON does not
   require a published frame and makes exclusive-capability nodes available
   to CAPTURE clients.
6. A CAPTURE client requests MMAP buffers, exports every index, queues the
   indices it can accept, and starts streaming.
7. Publish real frames with OUTPUT QBUF(DMABUF), using the bound dma-buf
   for each index. Duplicate fds referring to the same object are accepted.

Binding does not queue a frame. Before all slots are bound, CAPTURE REQBUFS
returns EAGAIN and OUTPUT STREAMON returns ENODATA. EXPBUF on an unbound slot
returns ENODATA, rather than exporting unrelated internal storage.

A binding cannot be replaced, even before streaming. The first OUTPUT QBUF
or STREAMON seals the bindings. They survive OUTPUT STREAMOFF and are dropped
by OUTPUT REQBUFS(0)/close; previously exported dma-bufs retain their own
references. The producer must not rebind an old allocation into a new pool
until all its old users have finished.

MMAP is the consumer's queue memory type. Pixel access uses mmap/import of
the **exported dma-buf fd**. Mapping the video node, read(), write(), timeout
images and frame duplication are unsupported in this mode. No pixel copying
is performed. Existing applications using EXPBUF can keep ordinary QBUF/DQBUF;
applications using video-node mmap/read need the legacy mode or an adapter.

## Queue ownership

For each index, the OUTPUT state is:

```text
IDLE -> PUBLISHED -> DELIVERED -> RELEASED -> IDLE
          |             |          |
          |             |          +-- OUTPUT DQBUF (normal completion)
          |             +------------- CAPTURE QBUF (release)
          +--------------------------- CAPTURE DQBUF (delivery)

PUBLISHED or DELIVERED -> CANCELLED -> RETIRED
                         STREAMOFF    OUTPUT DQBUF with ERROR
                         or close
```

CAPTURE also records whether the index has been queued as available. Initial
QBUF sets availability without releasing anything. DQBUF requires both a
published frame and availability, and removes availability. The later QBUF
releases that frame and restores availability for the next one.

- OUTPUT QBUF on a non-IDLE slot returns EBUSY.
- With `V4L2LOOPBACK_CONSUMER_SYNC_TRY` (2), OUTPUT QBUF returns EAGAIN
  without publishing unless CAPTURE is streaming and that index is queued
  as available. The availability check and publication are atomic with
  respect to CAPTURE stop/close. A late reader receives fresh frames instead
  of occupying its producer's credits with stale startup frames.
- `V4L2LOOPBACK_CONSUMER_SYNC_WAIT` (1) allows publication before a reader
  queues the index or starts streaming. The pending publication waits for
  a reader; the OUTPUT QBUF ioctl itself does not wait. OFF (0) disables
  consumer synchronization. Select the mode before requesting buffers.
- CAPTURE QBUF on an already queued slot returns EINVAL. In particular, a
  repeated release cannot complete a newly published frame before its DQBUF.
- CAPTURE DQBUF delivers published frames in publication order among
  available slots; it does not complete OUTPUT.
- OUTPUT DQBUF returns only released or cancelled buffers. With no consumer
  it waits, or returns EAGAIN on an O_NONBLOCK fd.
- poll reports CAPTURE readiness for delivery and OUTPUT readiness for a
  release/cancellation. Blocking DQBUF is interruptible and wakes on stop.
- QUERYBUF reports each direction's queue flags independently.

The consumer must finish **all** CPU, GPU and display access before release
QBUF. These ioctls do not wait for renderer/display fences. DMA_BUF_IOCTL_SYNC
only brackets CPU/cache access; it does not prove GPU completion.

Standard V4L2 QBUF has no release-generation input. Queue state rejects
duplicate QBUF while queued and retains cancelled slots across reconnects.
It cannot distinguish a deliberately replayed old QBUF after a newer DQBUF
on the same file description from a valid release. Consumers must serialize
ownership and return each delivered buffer once. This is a cooperative
consumer contract, not isolation against arbitrary access through exported
writable dma-bufs.

## Stop, cancellation and a new pool

CAPTURE STREAMOFF, REQBUFS(0) and close turn every PUBLISHED/DELIVERED slot
into CANCELLED, including a publication never dequeued by userspace. OUTPUT
DQBUF returns those slots with V4L2_BUF_FLAG_ERROR and permanently retires
them for this binding epoch. A producer must quarantine their allocations;
ERROR is **not** permission to overwrite them. A normal release already
recorded before stop remains a normal completion.

After CAPTURE restart, unretired slots still work. Retired slots may be
initially queued by the new reader, but OUTPUT cannot publish them again.
There is deliberately no timeout or reset ioctl that turns cancellation
into a safe release. A producer distributing a global pool must keep these
loans charged to the failed consumer's credit budget.

OUTPUT STREAMOFF ends the entire pool epoch. DQBUF on either side reports
EPIPE, poll reports POLLERR, and OUTPUT cannot STREAMON again. Destroy both
queues with REQBUFS(0)/close before setting up a new pool. The mode resets
once neither direction owns the format. A blocked DQBUF also detects a
CAPTURE stream epoch change, so stop/restart cannot hand it a new frame.

## Test

Build and load this module on unused nodes. For example, with system dma-heap:

```sh
make
make -C tests test_consumer_sync test_dmabuf
sudo insmod v4l2loopback.ko devices=1 video_nr=10 max_buffers=16 exclusive_caps=1
sudo tests/test_consumer_sync /dev/video10 /dev/dma_heap/system
sudo tests/test_dmabuf /dev/video10
sudo rmmod v4l2loopback
```

The tests cover prebinding without fake frames, shared object identity,
exclusive CAPTURE ownership, initial availability, exact queue-state release,
duplicate QBUF, 1,000 reuses, blocking/nonblocking/poll, signal interruption,
both STREAMOFF directions, close cancellation, retirement across reconnects,
export lifetime and legacy write/read/DMABUF compatibility.
TRY-mode checks also cover absent, queued-but-not-streaming, unavailable,
stopped and closed readers, with no publication on EAGAIN.
