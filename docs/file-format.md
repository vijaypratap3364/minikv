# MiniKV File Format

This document defines binary record format version 2. A MiniKV segment is zero
or more records concatenated without a separate file header. Every record carries
its own magic, version, operation, lengths, and checksum so a reader can validate
the boundary it was given.

## Version 2 record layout

| Byte offset | Width | Field | Version 2 rule |
| ---: | ---: | --- | --- |
| 0 | 4 bytes | Magic | ASCII bytes `MKVR` |
| 4 | 1 byte | Format version | `0x02` |
| 5 | 1 byte | Operation | `0x01` PUT; `0x02` DELETE |
| 6 | 2 bytes | Reserved | Both bytes must be zero |
| 8 | 4 bytes | Key length | Unsigned 32-bit little-endian byte count |
| 12 | 4 bytes | Value length | Unsigned 32-bit little-endian byte count |
| 16 | Key length | Key | Uninterpreted bytes |
| 16 + key length | Value length | Value | Uninterpreted bytes |
| 16 + key length + value length | 4 bytes | Checksum | CRC-32/ISO-HDLC, unsigned little-endian |

All multibyte integers use little-endian byte order: the least significant byte
appears first. Magic, version, and operation fields do not have an endianness.
The fixed header is 16 bytes and the trailing checksum is 4 bytes.

A PUT may have an empty key or value. A DELETE is a tombstone: it contains the
key being deleted and must have a zero value length. Empty and binary-safe keys
remain valid for both operations.

## Checksum

Version 2 uses CRC-32/ISO-HDLC, commonly called the standard CRC-32 used by ZIP
and Ethernet. Its parameters are:

- reflected polynomial `0xEDB88320`
- initial value `0xFFFFFFFF`
- reflected byte processing
- final XOR `0xFFFFFFFF`
- check value for ASCII `123456789`: `0xCBF43926`

The checksum covers every byte from the `M` in the magic through the final key
or value byte. It does not include the four checksum bytes themselves. The
decoder recomputes and compares it before constructing key and value strings.

CRC-32 detects many common accidental changes, including flipped bytes and
short bursts of corruption. It is not cryptography: an attacker can deliberately
change data and calculate a matching checksum. It also cannot repair corrupted
bytes, identify which copy is correct, or prove that flushed data reached stable
storage before a power loss.

## Limits

- Maximum key length: 65,536 bytes (64 KiB)
- Maximum PUT value length: 4,194,304 bytes (4 MiB)
- DELETE value length: exactly zero bytes
- Maximum encoded record length: 4,259,860 bytes, including header and checksum

The length fields are 32 bits so the format has room to evolve, but version 2
enforces smaller operational limits to bound allocation and keep the educational
engine safe on the constrained development machine.

## Versioning

The encoder writes only version 2 and the decoder accepts only version 2.
Version 1 had the same 16-byte header and PUT payload but no checksum or DELETE
tombstone. Because version 1 records cannot provide Stage 4's integrity
guarantee, they are rejected as unsupported rather than guessed or silently
upgraded. An incompatible future layout must use another version and an explicit
decoder.

The reserved bytes must remain zero in version 2 so future flags cannot be
silently misinterpreted by an older reader.

## Validation and errors

Before returning a record, the decoder:

1. Requires the complete 16-byte header.
2. Verifies magic, version, operation, and zero reserved bytes.
3. Decodes both lengths explicitly from little-endian bytes.
4. Enforces key/value limits and the zero-length DELETE value rule.
5. Computes the bounded total record size and requires all payload and checksum
   bytes.
6. Recomputes CRC-32 and compares it with the stored checksum.

Invalid magic, version, operation, reserved bytes, lengths, or tombstone shape
produce an invalid-format error. Missing header, payload, or checksum bytes
produce an incomplete-record error. A structurally complete record whose CRC-32
does not match produces a checksum-mismatch error. A missing key is different:
it is valid database state and `GET` returns `std::nullopt`.

The decoder returns the number of bytes consumed, allowing a caller to advance
to the next record. Extra bytes after one complete record are not an error
because they may begin the next record.

## Segment container layout

Record format version 2 did not change when segmentation was introduced. The
container around those records is now a database directory:

```text
example.minikv/
  CURRENT
  generation-00000000000000000001/
    segment-00000000000000000001.dat
    segment-00000000000000000002.dat
```

`CURRENT` is a small text manifest with its own layout version and the selected
generation identifier. Its current form is `MINIKV-MANIFEST 1 <generation>`,
followed by a newline. Segment and generation identifiers are unsigned 64-bit
values rendered as 20 decimal digits so lexical order matches numeric order.
Segment identifiers start at one and must be contiguous. The highest numbered
segment is active; all earlier files are immutable.

The maximum segment size is a rollover target, not a record-format limit. A new
record goes to a fresh segment when it would make a nonempty active segment
exceed the configured target. One valid record may itself be larger than that
target and occupies a segment by itself.

Compaction writes `generation-<id>.tmp`, closes and optionally syncs its segment
files, then atomically renames it to the final generation name. After read-back
validation, MiniKV writes `CURRENT.tmp` and atomically replaces `CURRENT`. Only
the selected generation participates in recovery. Recognized temporary and
unselected generation directories are safe to remove after the selected
generation has opened and validated. The manifest layout version is independent
of binary record format version 2.

## Offsets, flushing, and current limits

An offset is a zero-based byte position within one segment. The location returned
by an append combines a segment identifier, the offset of the `M` in that
record's magic, and the encoded record size. The in-memory index stores that
complete location to find live values without scanning unrelated records.

The storage log opens files in binary append mode and flushes the C++ stream
after each record. In buffered durability mode, that is the write-completion
boundary before MiniKV changes its index. In sync mode, MiniKV additionally calls
`FlushFileBuffers` on Windows or `fsync` on POSIX before changing the index.

Startup validates each selected-generation segment from offset zero. If the
active segment ends before the next record's header, payload, or checksum is
complete, MiniKV truncates that segment to its last complete checksum-valid
record and recovers all earlier state. The same incomplete input in an immutable
segment is corruption and remains untouched. Invalid format or checksum mismatch
in a complete record also aborts opening. No recovery path searches for a guessed
later boundary.
