# SSTableV2

## Scope

The first production slice implements a faithful key-file/value-file pipeline:

- user rows are `row key columns + Version + OpType + one Value`;
- values larger than `MaxEmbeddedValueSizeInByte` are written to a value file;
- key files are append-only and immutable after `finish()`;
- key files contain data blocks, a root index block, metadata sections, a
  locator, and a fixed 32-byte tail;
- data blocks and index blocks use the PDF's common 52-byte block header;
- column-store cells use pattern 0, the PDF's no-pattern representation.

Pattern 1-5 encoders, multi-round prefix compression, and multi-level indexes
are extension points. They must preserve the same block, locator, and tail
wire format.

## Implementation Progress

Status as of the current implementation pass:

| Area                      | Status                            | Notes                                                                                                                    |
| ------------------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| Strong schema and row API | Implemented                       | `types/table_schema.h` defines `TableSchema`, `StructuredRowKey`, and `Row`. `SSTableBuilder` consumes `types::Row`.     |
| Tail                      | Implemented                       | 32-byte PDF tail with checksum, locator offset/length, version 2, and `SST\0` magic.                                     |
| Locator                   | Implemented                       | `LOCA` metadata-section shape with checksum and `map<string, UInt64>` payload.                                           |
| Metadata sections         | Implemented                       | Configuration, Schema, Statistics, and Compatibility sections use metadata-section shape.                                |
| Data block                | Implemented                       | Common 52-byte block header and body ordering: data table, column-store units, offset table. Column units use pattern 0. |
| Root index                | Implemented                       | Root index is a `ROOT` block pointing to data blocks with `@2` current-file semantics.                                   |
| Value file                | Implemented                       | Values larger than threshold are stored in a separate value file with offset/length/checksum in the key file.            |
| Reader                    | Implemented                       | Opens via tail -> locator -> root index, scans data blocks, and reads embedded/separated values.                         |
| Bloom filters             | Implemented                       | File layer emits PDF `BLOM` section and `BloomFilter0_Offset/Length` locator entries.                                    |
| Pattern 1-5               | Not yet implemented in this slice | Pattern 0 is valid and used for all column units.                                                                        |
| Multi-level index         | Not yet implemented in this slice | Current root index points directly to data blocks.                                                                       |

Current verification:

```text
bazel test //cpp/pl/sstv2/ut:file_test
bazel test //cpp/pl/sstv2/ut:all
```

`file_test` covers PDF map locator round-trip, PDF tail round-trip, embedded
values, separated values, root-index discovery, bloom locator entries and
membership checks, metadata locator entries, and reader value checksum
validation. The full SSTableV2 test suite currently
passes: 11/11 Bazel test targets.

## Current Code Map

| File                                                 | Role                                                                                                          |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `types/table_schema.h`                               | PDF-facing strong schema and row API: `TableSchema`, `StructuredRowKey`, `Row`.                               |
| `file/tail.h`, `file/tail.cpp`                       | PDF v0.95 fixed tail encoding/decoding and checksum validation.                                               |
| `file/locator.h`, `file/locator.cpp`                 | PDF v0.95 locator map encoding/decoding.                                                                      |
| `file/sstable_builder.h`, `file/sstable_builder.cpp` | Strong-schema builder that writes key file, value file, data blocks, root index, metadata, locator, and tail. |
| `file/sstable_reader.h`, `file/sstable_reader.cpp`   | Reader that opens via tail/locator/root-index and scans data blocks.                                          |
| `ut/file/file_test.cpp`                              | End-to-end tests for the PDF-compatible file pipeline.                                                        |

## Remaining Work

The current implementation restores the core file format and read/write
pipeline. Remaining work for complete v0.95 coverage:

- plug pattern selectors 1-5 into column-store unit generation while preserving
  pattern 0 as fallback;
- implement multi-level `IXBK` index blocks for very large files;
- expose typed row decoding from `StoredRow` back to `types::Row`;
- support row-count-key metadata and multiple bloom filter keys;
- replace the older draft `block/`, `index/`, `metadata/`, and `bloom/` modules
  with shared PDF-format primitives or retire them after migration.

## User Model

The user-facing schema has:

- one or more row-key columns, ordered ascending;
- fixed system-key columns:
  - `Version`, type `Version`, ordered descending;
  - `OpType`, type `UInt8`, ordered ascending;
- one logical `Value` cell, with per-row type carried by the internal `Flag`.

The internal row stored in the key file is:

```text
row-key columns...
Version
OpType
Flag
Filename
Offset
Length
Checksum
```

For embedded values, `Filename` is empty and `Offset/Length` point into the
data block's data table. For separated values, `Filename` is the value-file
path and `Offset/Length/Checksum` locate the value bytes in that file.

## Key File Layout

The key file is written as:

```text
DataBlock*
RootIndexBlock
MetadataSection(Configuration)
MetadataSection(Schema)
MetadataSection(Statistics)
MetadataSection(Compatibility)
Locator
Tail
```

The PDF allows bloom filters, metadata, and locator sections to appear in any
order and allows padding between them. This implementation writes them
contiguously for simplicity; readers must still use the locator.

## Common Block Format

All blocks begin with a 52-byte fixed header:

```text
uint32 magic              // DTBK, IXBK, or ROOT
uint64 flags              // PS/RK/PC/C bits from the PDF
uint64 row_count
uint64 offset_table_offset
uint64 uncompressed_size
uint64 compressed_size    // 0 when uncompressed
uint64 checksum           // CRC32C widened to uint64, checksum field zeroed
```

The body is:

```text
data table
column-store unit*
optional row-key bitmap
offset table
```

The offset table is a varint-encoded sequence of absolute offsets relative to
the block start. It contains one entry per column-store unit, plus one bitmap
entry when the RK bit is set.

Column-store units use pattern 0:

```text
uint8  pattern_id = 0
varint row_count
cell*
```

Each cell is either a fixed-width integer-like value or an `offset,length` pair
into the data table, depending on the column type derived from schema.

## Index

The root index is itself a block with magic `ROOT`. Its rows point to data
blocks using the PDF's private `DataBlock` value semantics. Each index row
contains:

```text
last_all_key
Filename = "@2"
Offset
Length
```

`@2` means "current key file", matching the PDF. A future multi-level index
will use `IXBK` intermediate blocks and private `IndexBlock` values.

## Locator

The locator uses the metadata-section shape:

```text
uint32 magic = LOCA
uint64 checksum
map<string, Variant>
```

Required keys:

- `RootIndex_Offset`
- `RootIndex_Length`
- `Configuration_Offset`
- `Configuration_Length`
- `Schema_Offset`
- `Schema_Length`
- `Statistics_Offset`
- `Statistics_Length`
- `Compatibility_Offset`
- `Compatibility_Length`

Optional keys include `BloomFilterN_Offset/Length` and
`UserDefinedData_Offset/Length`.

## Tail

The tail is exactly 32 bytes and is always the last bytes of the key file:

```text
uint64 checksum           // tail checksum, checksum field zeroed
uint64 locator_offset
uint64 locator_length
uint32 version = 2
uint32 magic = 0x00545353 // "SST\0"
```

Readers start by reading these 32 bytes, validating the checksum/version/magic,
then loading the locator.
