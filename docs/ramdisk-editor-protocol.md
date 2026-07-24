# Ramdisk editor protocol

Two commands expose the same persistent in-memory CPIO protocol:

```text
ksud ramdisk-editor <ramdisk.cpio>
ksud boot-ramdisk-editor <source.img> <output.img>
```

The boot-image form accepts standard AOSP boot/init_boot header v3-v4 images.
It decompresses the ramdisk directly into the document and rebuilds the image
without creating `kernel`, `ramdisk.cpio`, or other unpacked workspace shards.
The original ramdisk compression and supported AVB footer layout are retained.

Both commands read request frames from standard input and write response frames
to the original standard output. Diagnostic output is redirected to standard
error.

Closing the process without `DUMP` discards all changes. In raw-CPIO mode,
`DUMP` atomically replaces the session CPIO. In boot-image mode it atomically
creates or replaces `output.img`; the source image remains untouched unless the
caller explicitly uses the same path. Neither mode writes a block device by
itself.

## Framing

All integers are unsigned little-endian values. Every request and response
starts with a 20-byte header:

| Field | Type | Value |
| --- | --- | --- |
| magic | 4 bytes | ASCII `YRCP` |
| version | `u16` | `1` |
| opcode | `u16` | request opcode; responses set bit `0x8000` |
| request ID | `u32` | echoed in the response |
| payload size | `u64` | bytes following the header |

Every response payload starts with a `u32` status:

| Value | Meaning |
| --- | --- |
| 0 | OK |
| 1 | invalid request |
| 2 | node not found |
| 3 | operation failed |
| 4 | I/O error |
| 5 | configured limit exceeded |
| 6 | unsupported protocol version |

A string is encoded as `u32 length` followed by exactly that many bytes.
Strings are UTF-8 at the Android boundary. An absent optional string uses
length `0xffffffff`.

## Node record

`STAT` and `LIST` return node records in this order:

```text
u64 id
u64 parent_id
u64 size
u32 inode
u32 mode
u32 uid
u32 gid
u32 nlink
u32 mtime_seconds
u32 dev_major
u32 dev_minor
u32 rdev_major
u32 rdev_minor
u8  synthetic_directory
string name
string normalized_path
optional-string symbolic_link_target
```

Node ID `0` is the synthetic archive root. IDs remain stable for the lifetime
of the process, including across rename and move operations.

## Requests

| Opcode | Name | Request payload | Successful response body |
| ---: | --- | --- | --- |
| 1 | HELLO | empty | `u32 version`, `u64 root_id`, `u64 max_content`, `u64 max_entries`, `u32 capabilities`, `u8 dirty` |
| 2 | STAT | `u64 id` | one node record |
| 3 | LIST | `u64 directory_id` | `u32 count`, then node records |
| 4 | READ | `u64 id`, `u64 offset`, `u64 length` | raw file bytes |
| 5 | REPLACE | `u64 id`, then raw replacement bytes | empty |
| 6 | CREATE_FILE | `u64 parent`, `u32 permissions`, `u32 uid`, `u32 gid`, `string name`, then raw content | `u64 created_id` |
| 7 | CREATE_DIRECTORY | `u64 parent`, `u32 permissions`, `u32 uid`, `u32 gid`, `string name` | `u64 created_id` |
| 8 | CREATE_SYMBOLIC_LINK | `u64 parent`, `u32 uid`, `u32 gid`, `string name`, `string target` | `u64 created_id` |
| 9 | CREATE_HARD_LINK | `u64 parent`, `u64 target_id`, `string name` | `u64 created_id` |
| 10 | COPY | `u64 id`, `u64 destination`, `string new_name` | `u64 created_id` |
| 11 | MOVE | `u64 id`, `u64 destination`, `string new_name` | empty |
| 12 | REMOVE | `u64 id`, `u8 recursive` | empty |
| 13 | UPDATE_METADATA | `u64 id`, `u32 mask`, then selected `u32` values | empty |
| 14 | DUMP | empty | empty |
| 15 | CLOSE | empty | empty, then the process exits |

The `HELLO` capability mask uses these bits:

| Bit | Capability |
| ---: | --- |
| 0 | read content |
| 1 | replace content |
| 2 | create regular file |
| 3 | create directory |
| 4 | create symbolic link |
| 5 | create hard link |
| 6 | copy |
| 7 | move or rename |
| 8 | remove |
| 9 | update metadata |
| 10 | atomic dump |
| 11 | ranged read |
| 12 | implicit-directory synthesis |

The metadata mask uses bit `1` for permission bits, `2` for UID, `4` for GID,
and `8` for mtime. Values occur in that order when their bits are present.

For streamed requests, the frame payload size is authoritative. A producer
must finish writing the complete frame before waiting for its response.
