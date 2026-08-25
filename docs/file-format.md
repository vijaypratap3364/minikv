# MiniKV File Format

This document defines binary record format version 1. A MiniKV log is zero or
more records concatenated without a separate file header. Every record carries
its own magic and version so a decoder validates the boundary it was given.

## Version 1 record layout

| Byte offset | Width | Field | Version 1 rule |
| ---: | ---: | --- | --- |
| 0 | 4 bytes | Magic | ASCII bytes `MKVR` |
| 4 | 1 byte | Format version | `0x01` |
| 5 | 1 byte | Operation | `0x01` PUT, `0x02` DELETE |
| 6 | 2 bytes | Reserved | Both bytes must be zero |
| 8 | 4 bytes | Key length | Unsigned 32-bit little-endian byte count |
| 12 | 4 bytes | Value length | Unsigned 32-bit little-endian byte count |
| 16 | Key length | Key | Uninterpreted bytes |
| 16 + key length | Value length | Value | Uninterpreted bytes |

All multibyte integers use little-endian byte order: the least significant byte
appears first. Magic, version, and operation fields do not have an endianness.
The fixed header is 16 bytes.

DELETE is a tombstone. Its key identifies the value to remove, and its value
length must be zero. Empty keys are valid for both operations. Empty PUT values
are valid and are distinct from DELETE.

## Limits

- Maximum key length: 65,536 bytes (64 KiB)
- Maximum PUT value length: 4,194,304 bytes (4 MiB)
- Maximum encoded record length: 4,259,856 bytes, including the header

The fields are 32 bits so the format has room to evolve, but version 1 enforces
smaller operational limits to bound allocation and keep the educational engine
safe on the constrained development machine.

## Versioning

The decoder accepts only version 1. An incompatible future layout must use a new
version number and an explicit decoder; unknown versions are rejected rather
than guessed. The reserved bytes must remain zero in version 1 so future flags
cannot be silently misinterpreted by an old reader.

## Validation

Before constructing key or value strings, the decoder:

1. Requires the complete 16-byte header.
2. Verifies magic, version, operation, and zero reserved bytes.
3. Decodes both lengths explicitly from little-endian bytes.
4. Rejects lengths above the version 1 limits.
5. Rejects a DELETE with a nonzero value length.
6. Computes the bounded total record size and requires that many input bytes.

The decoder returns the number of bytes consumed, allowing a caller to advance
to the next record. Extra bytes after one complete record are not an error
because they may begin the next record.

## Offsets, flushing, and current limits

An offset is a zero-based byte position in the log. The offset returned by an
append points to the `M` in that record's magic. Offsets let later stages refer
to records without rewriting or copying earlier bytes.

The storage log opens files in binary append mode and flushes the C++ stream
after each record. This makes append errors observable before MiniKV changes its
in-memory map, but it is not an operating-system sync barrier and therefore is
not yet a power-loss durability guarantee.

Version 1 has no checksum. The decoder rejects truncated input, but the engine
does not yet scan, recover, or repair a log on startup. Checksums, torn-tail
handling, explicit sync policy, and restart replay are later milestones.
