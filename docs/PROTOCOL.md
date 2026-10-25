# Protocol

This page is everything a server needs to implement. Over MQTT that is about
60 lines in any language with an MQTT client; `OtaServer` in this library is
the same server side in C++.

## Binary frames

Every transport uses the same frames. They're little-endian, and every frame
starts with the same six bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type: 1 Offer, 2 Request, 3 Chunk, 4 Status, 5 Command |
| 1 | 1 | protocol revision, currently `1`. A receiver refuses any other revision |
| 2 | 4 | job: the server's number for this transfer; never 0 |

### Request (node → server), 16 bytes

| Offset | Size | Field |
|---|---|---|
| 6 | 4 | offset |
| 10 | 4 | length: send `[offset, offset + length)` |
| 14 | 2 | max chunk: the largest chunk payload the node's link carries |

The server slices the range into chunks no bigger than `max chunk` (and its own
limit) and sends them in order. A node may send a new request before the last
one is finished. If its offset is where the previous range ended, it extends
that range; otherwise it replaces it (the node is asking again from where it
really is).

### Chunk (server → node), 14 + n bytes

| Offset | Size | Field |
|---|---|---|
| 6 | 4 | offset of the first byte |
| 10 | 4 | CRC-32 (zlib, `0xEDB88320` reflected) of the payload |
| 14 | n | payload |

A node keeps a chunk only if its offset is exactly the next one expected and
its CRC matches. Anything else is dropped; a gap triggers an immediate
re-request.

### Offer (server → node)

| Offset | Size | Field |
|---|---|---|
| 6 | 4 | image size |
| 10 | 1 | flags: `0x01` manual apply, `0x02` force |
| 11 | 1 | signature length: 0 (unsigned) or 64 |
| 12 | 32 | SHA-256 of the image |
| 44 | 1 + v | version, length-prefixed, < 32 bytes |
| … | 1 + b | board, length-prefixed, < 32 bytes |
| … | s | signature |

The frame ends exactly after the signature; a frame with bytes left over is
refused.

### Status (node → server)

| Offset | Size | Field |
|---|---|---|
| 6 | 1 | state: 0 idle, 1 offered, 2 downloading, 3 verifying, 4 ready, 5 applying, 6 testing, 7 checking |
| 7 | 1 | outcome: 0 none, 1 success, 2 failed |
| 8 | 1 | result: `OtaResult` (see below) |
| 9 | 1 | reserved, 0 |
| 10 | 4 | bytes received |
| 14 | 4 | image size |
| 18 | 1 + r | running version, length-prefixed |

### Command (server → node), 7 bytes

| Offset | Size | Field |
|---|---|---|
| 6 | 1 | 1 abort, 2 apply, 3 confirm, 4 rollback, 5 status |

Job 0 means "whatever is running". A command for any other job is ignored,
except `status`, which is always answered.

## The signature

The signature covers exactly these 114 bytes:

```
"modbee-ota-v1" 0x00 | sha256[32] | size u32 LE | board[32] | version[32]
```

Board and version are zero padded (so each is at most 31 bytes).
`ota_signedMessage()` builds it on the node. The node's verifier decides the
scheme; both signatures are 64 bytes:

| Verifier | Signature | Public key on the node |
|---|---|---|
| `OtaEd25519Verifier` | Ed25519 over the 114 bytes | 32 bytes, raw |
| `OtaEcdsaP256Verifier` | ECDSA P-256 over SHA-256 of the 114 bytes, `r` then `s`, 32 bytes each, big-endian | 64 bytes, `X` then `Y` |

Put it in the offer as hex (JSON `"signature"`) or raw (binary Offer frame).
Make a key once (`openssl genpkey -algorithm ed25519 -out ota-key.pem`, or
`-algorithm EC -pkeyopt ec_paramgen_curve:P-256` for P-256), then sign each
image. In Python, with the `cryptography` package:

```python
import hashlib, struct
from cryptography.hazmat.primitives import serialization

def signed_message(image: bytes, board: str, version: str) -> bytes:
    pad = lambda text: text.encode().ljust(32, b"\0")
    return (b"modbee-ota-v1\0" + hashlib.sha256(image).digest()
            + struct.pack("<I", len(image)) + pad(board) + pad(version))

image = open("firmware.bin", "rb").read()
key = serialization.load_pem_private_key(open("ota-key.pem", "rb").read(), None)
signature = key.sign(signed_message(image, "my-board", "1.0.1"))   # Ed25519
public_key = key.public_key().public_bytes(serialization.Encoding.Raw,
                                           serialization.PublicFormat.Raw)
```

Keep the private key off the devices and out of version control.

## MQTT topics

Under a base chosen per node, for example `modbee/node-uio-1/ota`:

| Topic | Direction | Payload | QoS / retain |
|---|---|---|---|
| `<base>/offer` | in | JSON offer, or a binary Offer frame | 1, may be retained |
| `<base>/data` | in | binary Chunk frames | 0 or 1 |
| `<base>/cmd` | in | `abort` `apply` `confirm` `rollback` `status`, or a Command frame | 1 |
| `<base>/req` | out | binary Request frames | 1, series (never superseded) |
| `<base>/status` | out | JSON status | 1, retained |

A server subscribes to `+/ota/req` and `+/ota/status` (or the equivalent
wildcard for its layout) to serve every node at once.

### JSON offer

```json
{"job": 17, "version": "1.4.0", "board": "modbee-node-uio", "size": 1048576,
 "sha256": "<64 hex>", "signature": "<128 hex>", "apply": "auto", "force": false}
```

`signature`, `apply` (`"auto"` or `"manual"`) and `force` are optional.
Unknown keys are skipped, so a platform can add its own (`notes`, `url`, …).

### JSON status

```json
{"state":"downloading","outcome":"none","result":"ok","job":17,
 "received":524288,"size":1048576,"progress":50,"board":"modbee-node-uio",
 "running":"1.3.2","offered":"1.4.0"}
```

The status is published on every state change, every second or so while
downloading, on every reconnect, and once after boot. After boot it reports
how the last job ended.

## Sparkplug B metrics

Two ways to use them. **Beside the MQTT topics** (OpenPLC's `OTA_SPARKPLUG`):
the image travels on `<base>/data` over the same connection, and Sparkplug
carries the status and the controls:

| Metric | Type | Direction |
|---|---|---|
| `Firmware/Version`, `Firmware/State`, `Firmware/Offered` | String | NDATA |
| `Firmware/Progress` | Int32, whole percent | NDATA |
| `Firmware/Status` | String (JSON status) | NDATA |
| `Node Control/Firmware Accept`, `Reject`, `Apply`, `Abort`, `Confirm`, `Rollback` | Boolean, TRUE to act | NCMD |

**Instead of them** (`OtaSparkplugLink`), for a host that can only speak
Sparkplug - the image rides in metrics:

| Metric | Type | Direction |
|---|---|---|
| `Node Control/Firmware Offer` | String (JSON offer) | NCMD |
| `Node Control/Firmware Chunk` | Bytes (Chunk frame) | NCMD |
| `Node Control/Firmware Command` | String | NCMD |
| `Firmware/Request` | Bytes (Request frame) | NDATA |
| `Firmware/Status` | String (JSON status) | NDATA |
| `Firmware/State`, `Firmware/Progress`, `Firmware/Version` | String, Int32, String | NDATA |

The host writes one chunk per request it sees. A metric only holds its latest
write, so the node never has more than one chunk in flight on this link.

## Security

A signature protects the image: with a verifier set, the node installs only
what the key signed, for its board, at that version. It does not protect the
commands (`abort`, `apply`, `confirm`, `rollback`), or the status the node
publishes. Use the broker's access control so only the server can write a
node's `offer`, `data` and `cmd` topics, and TLS where the network is not
trusted.

## Result codes

`0 ok, 1 pending, 2 busy, 3 invalid_argument, 4 not_ready, 5 no_memory,
6 unsupported, 7 io_error, 8 wrong_board, 9 too_large, 10 same_version,
11 downgrade, 12 unsigned, 13 bad_signature, 14 refused, 15 timeout,
16 bad_chunk, 17 hash_mismatch, 18 bad_image, 19 aborted, 20 rolled_back,
21 not_confirmed`.

Append only: the numbers are in Status frames and in the OpenPLC
`OTA_RESULT` datatype.
