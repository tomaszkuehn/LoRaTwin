# MeshCore Over-the-Air Protocol

## Based on the official [`meshcore.js`](https://github.com/meshcore-dev/meshcore.js) reference implementation

> **Source files analysed:** `packet.js`, `advert.js`, `meshcore_path.js`, `constants.js`, `connection.js`, `buffer_reader.js`, `buffer_writer.js`, `cayenne_lpp.js`
>
> **Version:** Companion protocol version 1 (`SupportedCompanionProtocolVersion = 1`)
>
> **LoRa physical layer:** 868 MHz (EU) / 915 MHz (US), SF8, BW 62.5 kHz, CR 4/8, sync word `0x12`

---

## 1. Over-the-Air Frame Structure

```
[SX1262 hardware layer: preamble | sync word 0x12 | explicit header | CRC]
                                     v stripped by hardware, readData() returns:
[Header 1B] [Transport Codes 4B?] [Path Length 1B] [Path N B] [Payload M B]
```

### 1.1 Header Byte

```
Bit:    7    6    5    4    3    2    1    0
     [ Version ] [  Payload Type  ] [RouteType]
       (2 bits)    (4 bits)         (2 bits)
```

| Field          | Bits  | Values                                                          |
|----------------|-------|-----------------------------------------------------------------|
| RouteType      | 1:0   | `0x00`=TransportFlood, `0x01`=Flood, `0x02`=Direct, `0x03`=TransportDirect |
| PayloadType    | 5:2   | `0x00`–`0x0F` (see §3)                                          |
| Version        | 7:6   | `0x00`=v1, `0x01`=v2, `0x02`=v3, `0x03`=v4                     |

**Special value:** `header = 0xFF` means "marked do-not-retransmit" — the packet has been forwarded and should not be repeated.

### 1.2 Route Types

| Code   | Name                  | Description                                               |
|--------|-----------------------|-----------------------------------------------------------|
| `0x00` | **TransportFlood**    | Flood mode with transport codes (4 extra bytes)           |
| `0x01` | **Flood**             | Flood mode, path built up hop-by-hop (max 64 bytes)       |
| `0x02` | **Direct**            | Direct route, path is pre-supplied                        |
| `0x03` | **TransportDirect**   | Direct route with transport codes                         |

### 1.3 Transport Codes (only for TransportFlood and TransportDirect)

When `routeType in {0x00, 0x03}`, 4 extra bytes follow the header:

| Offset | Size        | Field              |
|--------|-------------|--------------------|
| +1     | 2 bytes LE  | `transportCode1`   |
| +3     | 2 bytes LE  | `transportCode2`   |

Transport codes are used for flood-scoping and transport-layer routing.

---

## 2. Path Encoding

After the header (and optional transport codes), one byte encodes both the hop count and hash size:

```
Path Length byte: [HashSize:2 bits][HopCount:6 bits]
```

| Field          | Bits  | Range                                                          |
|----------------|-------|----------------------------------------------------------------|
| HopCount       | 5:0   | 0–63 hops                                                      |
| HashSize code  | 7:6   | `0x00`->1B, `0x01`->2B, `0x02`->3B, `0x03`->4B (reserved)     |

Hash size formula: `hashSize = (pathLenByte >> 6) + 1`

Path data follows immediately: `hopCount x hashSize` bytes.

Each hash identifies one hop (one node) in the path. The first hash is the origin, the last is the most recent forwarder.

**Special value:** `pathLenByte = 0xFF` means invalid/no path.

**Maximum path size:** 64 bytes.

---

## 3. Payload Types

| Code   | Name           | Routing        | Description                                    |
|--------|----------------|----------------|------------------------------------------------|
| `0x00` | **REQ**        | Direct         | Request (dest/src hashes + encrypted blob)     |
| `0x01` | **RESPONSE**   | Direct         | Response to REQ or ANON_REQ                    |
| `0x02` | **TXT_MSG**    | Flood/Direct   | Text message (plain or encrypted)              |
| `0x03` | **ACK**        | Direct         | Acknowledgment                                 |
| `0x04` | **ADVERT**     | Flood          | Node identity advertisement (beacon)           |
| `0x05` | **GRP_TXT**    | Flood          | Group text message (channel)                   |
| `0x06` | **GRP_DATA**   | Flood          | Group datagram (channel)                       |
| `0x07` | **ANON_REQ**   | Direct         | Anonymous request (ephemeral pubkey)           |
| `0x08` | **PATH**       | Direct         | Returned path info                             |
| `0x09` | **TRACE**      | Direct         | Path trace (collect SNR per hop)               |
| `0x0A` | **MULTIPART**  | --             | Multi-part message fragment                    |
| `0x0B` | **CONTROL**    | --             | Control messages (discovery, etc.)             |
| `0x0F` | **RAW_CUSTOM** | --             | Custom application data (raw bytes)            |

---

## 4. Payload Formats

All multi-byte integers are **little-endian** unless noted otherwise.

### 4.1 REQ (`0x00`)

Request — encrypted point-to-point message.

```
[dest 1B] [src 1B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0      | 1    | `dest` — destination node hash (first byte of public key)       |
| 1      | 1    | `src` — source node hash (first byte of public key)             |
| 2      | 2    | `cipher MAC` — 2-byte MAC for the encrypted ciphertext (uint16 LE) |
| 4      | N    | `ciphertext` — encrypted payload (timestamp + request data)     |

**Minimum size:** 4 bytes. **Payload version v1** uses 1-byte hashes and 2-byte MAC.

### 4.2 RESPONSE (`0x01`)

Response to a REQ or ANON_REQ.

```
[dest 1B] [src 1B] [cipher MAC 2B] [ciphertext N B]
```

Same structure as REQ. The ciphertext contains the response data.

### 4.3 TXT_MSG (`0x02`)

**Unencrypted (LoRaTwin own TX format, for testing):**

```
[timestamp 4B LE] [txtType:2b | attempt:2b 1B] [text N B]
```

| Offset | Size | Field                                                  |
|--------|------|--------------------------------------------------------|
| 0      | 4    | `timestamp` — seconds (boot-relative or Unix epoch)    |
| 4      | 1    | `txtType` (bits 7:2) + `attempt` (bits 1:0)           |
| 5      | N    | `text` — message content (printable ASCII)             |

Text types: `0`=Plain, `1`=CLI, `2`=SignedPlain

**Detection heuristic** (to distinguish from encrypted format):
1. `txtType <= 2` (upper 6 bits of byte 4 must be 0, 1, or 2)
2. `attempt <= 3` (lower 2 bits of byte 4)
3. >=75% of first 8 text bytes (or full text if shorter) are printable ASCII or whitespace

This gives a ~0.13% false-positive rate against random ciphertext.

**Encrypted (standard MeshCore nodes):**

```
[dest 1B] [src 1B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0      | 1    | `dest` — destination node hash                                  |
| 1      | 1    | `src` — source node hash                                        |
| 2      | 2    | `cipher MAC` — MAC for the encrypted ciphertext (uint16 LE)     |
| 4      | N    | `ciphertext` — encrypted: `[timestamp 4B] [txtType+attempt 1B] [text ...]` |

**Minimum size:** 4 bytes (dest + src + MAC).

### 4.4 ACK (`0x03`)

```
[ack_code 4B]
```

A 4-byte acknowledgment code (CRC checksum of message timestamp, text, and sender pubkey).

### 4.5 ADVERT (`0x04`)

Full node identity advertisement:

```
[publicKey 32B] [timestamp 4B LE] [signature 64B] [appData N B]
```

| Offset | Size | Field                                                              |
|--------|------|--------------------------------------------------------------------|
| 0      | 32   | `publicKey` — ED25519 public key                                   |
| 32     | 4    | `timestamp` — Unix timestamp of advert generation                  |
| 36     | 64   | `signature` — ED25519 signature over (pubkey + timestamp + appData)|
| 100    | N    | `appData` — application data (see below)                           |

#### 4.5.1 ADVERT AppData

```
[flags 1B] [lat 4B?] [lon 4B?] [feat1 2B?] [feat2 2B?] [name string?]
```

**Flags byte:**

```
Bit 7   6   5   4   3   2   1   0
  [NAME][F2][F1][LATLON][--- Type nibble ---]
```

| Bit | Mask    | Meaning                                                    |
|-----|---------|------------------------------------------------------------|
| 3:0 | `0x0F`  | **Type**: `0`=NONE, `1`=CHAT, `2`=REPEATER, `3`=ROOM, `4`=SENSOR |
| 4   | `0x10`  | **LATLON present** — 8 bytes follow (lat + lon, int32 LE)  |
| 5   | `0x20`  | **FEAT1 present** — 2 bytes follow (uint16 LE)             |
| 6   | `0x40`  | **FEAT2 present** — 2 bytes follow (uint16 LE)             |
| 7   | `0x80`  | **NAME present** — remainder of appData is a UTF-8 string  |

**Lat/Lon encoding:** `int32` micro-degrees. Divide by 1,000,000 to get decimal degrees.
Example: `lat = 52416160` -> `52.416160`

### 4.6 GRP_TXT (`0x05`)

Group text message — sent to all members of a channel. Encrypted with the channel's AES-256-GCM key.

```
[channelHash 1B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0      | 1    | `channelHash` — first byte of SHA256 of the channel's shared key|
| 1      | 2    | `cipher MAC` — 2-byte MAC for the encrypted ciphertext (uint16 LE) |
| 3      | N    | `ciphertext` — AES-256-GCM encrypted payload                    |

**Minimum size:** 3 bytes.

The plaintext inside the ciphertext matches the TXT_MSG format:
```
[timestamp 4B LE] [txtType:2b | attempt:2b 1B] [text N B]
```
The message is of the form `<sender name>: <message body>` (e.g., `user123: I'm on my way`).

**Routing:** Flood — all channel members forward the message.

### 4.7 GRP_DATA (`0x06`)

Group datagram — same structure as GRP_TXT but carries binary application data.

```
[channelHash 1B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                       |
|--------|------|-------------------------------------------------------------|
| 0      | 1    | `channelHash` — first byte of SHA256 of channel's shared key|
| 1      | 2    | `cipher MAC` — 2-byte MAC (uint16 LE)                       |
| 3      | N    | `ciphertext` — AES-256-GCM encrypted binary payload         |

**Minimum size:** 3 bytes.

The ciphertext plaintext format:

| Field      | Size | Description                                          |
|------------|------|------------------------------------------------------|
| data type  | 2    | Identifier for the type of data (uint16 LE)          |
| data len   | 1    | Byte length of data                                  |
| data       | rest | Application-specific binary data                     |

**Routing:** Flood — same as GRP_TXT.

### 4.8 ANON_REQ (`0x07`)

Anonymous request — uses an ephemeral keypair instead of revealing the sender's identity.

```
[dest 1B] [ephemeralPubKey 32B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0      | 1    | `dest` — destination node hash                                  |
| 1      | 32   | `ephemeralPubKey` — one-time ED25519 public key                 |
| 33     | 2    | `cipher MAC` — 2-byte MAC for the encrypted ciphertext (uint16 LE) |
| 35     | N    | `ciphertext` — encrypted request data                           |

**Minimum size:** 36 bytes (1 + 32 + 2 + at least 1 byte encrypted).

Common ciphertext formats:
- **Room server login:** `[timestamp 4B] [sync_timestamp 4B] [password ...]`
- **Repeater/Sensor login:** `[timestamp 4B] [password ...]`

### 4.9 PATH (`0x08`)

Returned path — describes the route a packet took from the original author.

```
[dest 1B] [src 1B] [cipher MAC 2B] [ciphertext N B]
```

| Offset | Size | Field                                                           |
|--------|------|-----------------------------------------------------------------|
| 0      | 1    | `dest` — destination node hash                                  |
| 1      | 1    | `src` — source node hash                                        |
| 2      | 2    | `cipher MAC` — 2-byte MAC for the encrypted ciphertext (uint16 LE) |
| 4      | N    | `ciphertext` — encrypted: `[path_len 1B] [path ...] [extra_type 1B] [extra ...]` |

**Minimum size:** 4 bytes. The ciphertext contains the path bytes and an optional extra payload (e.g., an embedded ACK).

### 4.10 TRACE (`0x09`)

```
[payload N B]
```

Path trace packet — collects SNR data for each hop along the route.

### 4.11 MULTIPART (`0x0A`)

```
[payload N B]
```

Multi-part message fragment. Assembled at the receiver.

### 4.12 CONTROL (`0x0B`)

```
[subType:4b | flags:4b 1B] [data N B]
```

| Offset | Size | Field                               |
|--------|------|-------------------------------------|
| 0      | 1    | `subType` (bits 7:4) + flags (bits 3:0) |
| 1      | N    | `data` — type-specific data         |

Known sub-types: `0x8`=DISCOVER_REQ, `0x9`=DISCOVER_RESP

### 4.13 RAW_CUSTOM (`0x0F`)

```
[customData N B]
```

Raw application bytes. For custom encryption schemes or application-specific protocols.

---

## 5. Path Hash Display

When displaying a packet path:

```
Path: A1B2C3 D4E5F6 1A2B3C
       ^       ^       ^
     hop 0   hop 1   hop 2
```

Each hash is `hashSize` bytes (1, 2, or 3). The number of hashes equals `hopCount`.

`pathLenByte = 0xFF` marks a path as invalid/do-not-retransmit.

---

## 6. ED25519 Signature Verification

ADVERT packets contain an ED25519 signature:

```
signature = sign(privateKey, publicKey[32] + timestamp[4] + appData[N])
```

Verification:
```
verify(publicKey[32], signature[64], (publicKey[32] + timestamp[4] + appData[N]))
```

The signature covers the public key itself, the timestamp, and all appData — preventing tampering with any advert field.

---

## 7. Companion Protocol (Serial/TCP/BLE)

The companion protocol runs between a host (PC, smartphone) and a MeshCore radio device.
Each message is prefixed with a direction byte.

### 7.1 Frame Types

| Byte           | Direction        | Meaning            |
|----------------|------------------|--------------------|
| `0x3E` (`>`)   | Host -> Radio    | Command            |
| `0x3C` (`<`)   | Radio -> Host    | Response / Push    |

### 7.2 Command Codes

Each command frame: `[cmdCode 1B] [params...]`

| Code   | Name                   | Parameters                                                                                     |
|--------|------------------------|------------------------------------------------------------------------------------------------|
| `0x01` | **AppStart**           | appVer(1B), reserved(6B), appName(string)                                                      |
| `0x02` | **SendTxtMsg**         | txtType(1B), attempt(1B), timestamp(4B LE), pubKeyPrefix(6B), text(string)                     |
| `0x03` | **SendChannelTxtMsg**  | txtType(1B), channelIdx(1B), timestamp(4B LE), text(string)                                    |
| `0x04` | **GetContacts**        | since(4B LE, optional)                                                                         |
| `0x05` | **GetDeviceTime**      | --                                                                                             |
| `0x06` | **SetDeviceTime**      | epochSecs(4B LE)                                                                               |
| `0x07` | **SendSelfAdvert**     | type(1B): `0`=ZeroHop, `1`=Flood                                                               |
| `0x08` | **SetAdvertName**      | name(string)                                                                                   |
| `0x09` | **AddUpdateContact**   | pubKey(32B), type(1B), flags(1B), outPathLen(1B), outPath(64B), advName(32B cstring), lastAdvert(4B LE), advLat(4B LE int32), advLon(4B LE int32) |
| `0x0A` | **SyncNextMessage**    | --                                                                                             |
| `0x0B` | **SetRadioParams**     | freq(4B LE), bw(4B LE), sf(1B), cr(1B)                                                         |
| `0x0C` | **SetTxPower**         | txPower(1B) dBm                                                                                |
| `0x0D` | **ResetPath**          | pubKey(32B)                                                                                    |
| `0x0E` | **SetAdvertLatLon**    | lat(4B LE int32 micro-deg), lon(4B LE int32 micro-deg)                                         |
| `0x0F` | **RemoveContact**      | pubKey(32B)                                                                                    |
| `0x10` | **ShareContact**       | pubKey(32B)                                                                                    |
| `0x11` | **ExportContact**      | pubKey(32B, optional — omit to export own identity)                                             |
| `0x12` | **ImportContact**      | advertPacketBytes(raw)                                                                         |
| `0x13` | **Reboot**             | reason(string)                                                                                 |
| `0x14` | **GetBatteryVoltage**  | --                                                                                             |
| `0x15` | **SetTuningParams**    | (todo)                                                                                         |
| `0x16` | **DeviceQuery**        | appTargetVer(1B)                                                                               |
| `0x17` | **ExportPrivateKey**   | --                                                                                             |
| `0x18` | **ImportPrivateKey**   | privateKey(32B)                                                                                |
| `0x19` | **SendRawData**        | pathLen(1B), path(var), rawData(var)                                                           |
| `0x1A` | **SendLogin**          | pubKey(32B), password(string, max 15 chars)                                                    |
| `0x1B` | **SendStatusReq**      | pubKey(32B)                                                                                    |
| `0x1F` | **GetChannel**         | channelIdx(1B)                                                                                 |
| `0x20` | **SetChannel**         | channelIdx(1B), name(32B cstring), secret(32B)                                                 |
| `0x21` | **SignStart**          | (crypto signing)                                                                               |
| `0x22` | **SignData**           | (crypto signing)                                                                               |
| `0x23` | **SignFinish**         | (crypto signing)                                                                               |
| `0x24` | **SendTracePath**      | --                                                                                             |
| `0x26` | **SetOtherParams**     | --                                                                                             |
| `0x27` | **SendTelemetryReq**   | reserved(3B), pubKey(32B)                                                                      |
| `0x32` | **SendBinaryReq**      | pubKey(32B), requestCodeAndParams(var)                                                         |
| `0x36` | **SetFloodScope**      | reserved(1B, must be 0), transportKey(var)                                                     |
| `0x38` | **GetStats**           | statsType(1B): `0`=Core, `1`=Radio, `2`=Packets                                                |
| `0x3E` | **SendChannelData**    | channelIdx(1B), pathLen(1B), path(var), dataType(2B LE), payload(var)                          |

### 7.3 Response Codes (Radio -> Host)

| Code   | Name                | Description                               |
|--------|---------------------|-------------------------------------------|
| `0x00` | **Ok**              | Command succeeded                         |
| `0x01` | **Err**             | Command failed                            |
| `0x02` | **ContactsStart**   | Beginning of contacts list                |
| `0x03` | **Contact**         | A single contact entry                    |
| `0x04` | **EndOfContacts**   | End of contacts list                      |
| `0x05` | **SelfInfo**        | Own node information                      |
| `0x06` | **Sent**            | Message sent confirmation                 |
| `0x07` | **ContactMsgRecv**  | Direct message received from a contact    |
| `0x08` | **ChannelMsgRecv**  | Message received on a channel             |
| `0x09` | **CurrTime**        | Current device time                       |
| `0x0A` | **NoMoreMessages**  | No more stored messages                   |
| `0x0B` | **ExportContact**   | Exported contact data                     |
| `0x0C` | **BatteryVoltage**  | Battery voltage reading                   |
| `0x0D` | **DeviceInfo**      | Device information                        |
| `0x0E` | **PrivateKey**      | Exported private key                      |
| `0x0F` | **Disabled**        | Feature disabled                          |
| `0x12` | **ChannelInfo**     | Channel information                       |
| `0x13` | **SignStart**       | Signing session started                   |
| `0x14` | **Signature**       | Signature result                          |
| `0x18` | **Stats**           | Statistics data                           |
| `0x1B` | **ChannelDataRecv** | Channel data received                     |

### 7.4 Push Codes (Radio -> Host, unsolicited)

Push codes have the high bit set (`0x80`):

| Code   | Name                  | Description                                    |
|--------|-----------------------|------------------------------------------------|
| `0x80` | **Advert**            | New node advert received (auto-add mode)       |
| `0x81` | **PathUpdated**       | Route path updated                             |
| `0x82` | **SendConfirmed**     | Transmission confirmed by ACK                  |
| `0x83` | **MsgWaiting**        | Message(s) waiting to be retrieved             |
| `0x84` | **RawData**           | Raw data received                              |
| `0x85` | **LoginSuccess**      | Login to repeater/room server succeeded        |
| `0x86` | **LoginFail**         | Login failed                                   |
| `0x87` | **StatusResponse**    | Status response from remote node               |
| `0x88` | **LogRxData**         | Received data for logging                      |
| `0x89` | **TraceData**         | Path trace data                                |
| `0x8A` | **NewAdvert**         | New advert received (manual-add mode)          |
| `0x8B` | **TelemetryResponse** | Telemetry data from sensor/repeater            |
| `0x8C` | **BinaryResponse**    | Binary request response                        |

### 7.5 Error Codes

| Code   | Name                | Description                           |
|--------|---------------------|---------------------------------------|
| `0x01` | **UnsupportedCmd**  | Command not recognized                |
| `0x02` | **NotFound**        | Requested item not found              |
| `0x03` | **TableFull**       | Contact/channel table full            |
| `0x04` | **BadState**        | Device in wrong state for command     |
| `0x05` | **FileIoError**     | File system error                     |
| `0x06` | **IllegalArg**      | Invalid argument                      |

---

## 8. Cayenne LPP Sensor Telemetry

Sensor nodes (ADVERT type = SENSOR, `0x04`) may transmit telemetry in **Cayenne LPP**
(Low Power Payload) format. The `cayenne_lpp.js` module provides encoding and decoding
for standard data channels:

| Channel | Type                 | Size | Description          |
|---------|----------------------|------|----------------------|
| 0       | Digital Input        | 1B   | 0 or 1               |
| 1       | Digital Output       | 1B   | 0 or 1               |
| 2       | Analog Input         | 2B   | 0.01 signed          |
| 3       | Analog Output        | 2B   | 0.01 signed          |
| 101     | Illuminance          | 2B   | 1 lux unsigned       |
| 102     | Presence             | 1B   | 0 or 1               |
| 103     | Temperature          | 2B   | 0.1 C signed         |
| 104     | Humidity             | 1B   | 0.5% unsigned        |
| 105     | Accelerometer        | 6B   | X/Y/Z 0.001 G        |
| 106     | Barometric Pressure  | 2B   | 0.1 hPa              |
| 113     | Gyrometer            | 6B   | X/Y/Z 0.01 /s        |
| 115     | GPS Location         | 9B   | lat/lon/alt          |
| 134     | Frequency            | 4B   | 1 Hz unsigned        |
| 136     | Voltage              | 2B   | 0.01 V unsigned      |

Each Cayenne LPP frame: `[channel 1B] [type 1B] [value NB] [channel 1B] [type 1B] [value NB] ...`

---

## 9. Binary Request Types

Used with `SendBinaryReq` command for querying remote nodes:

| Code   | Name                 | Description                      |
|--------|----------------------|----------------------------------|
| `0x03` | **GetTelemetryData** | Request sensor telemetry         |
| `0x04` | **GetAvgMinMax**     | Request average/min/max stats    |
| `0x05` | **GetAccessList**    | Request node access list         |
| `0x06` | **GetNeighbours**    | Request neighbor table           |

---

## 10. LoRa Physical Layer Constants

| Parameter             | Value                                    |
|-----------------------|------------------------------------------|
| Sync Word             | `0x12`                                   |
| Spreading Factor      | 8                                        |
| Bandwidth             | 62.5 kHz                                 |
| Coding Rate           | 4/8                                      |
| Default Frequency     | 869.618 MHz (EU)                         |
| Preamble Length       | 16 symbols                               |
| Max Packet Payload    | 184 bytes (SF8, BW62.5, explicit header) |

---

## 11. Buffer Serialization Convention

All protocol buffers use:

- **Multi-byte integers:** Little-endian (LE)
- **Integers:** Unsigned unless noted (`int32` for signed lat/lon)
- **Strings:** UTF-8, length-delimited by remaining buffer space or null-terminated (cstring)
- **CStrings:** Fixed-size buffer, null-terminated. Trailing bytes after null are ignored.
- **Public keys:** 32 bytes raw binary (ED25519)
- **Signatures:** 64 bytes raw binary (ED25519)

---

## 12. Transport Key

The transport key is used for flood scoping via `SetFloodScope`. Stored as raw bytes,
typically derived from a shared secret. Length varies by implementation.

---

## 13. Data Type Namespace

`0xFFFF` — Developer namespace for experimenting with group/channel datagrams and
building custom applications.

---

## 14. Statistics Types

Used with `GetStats`:

| Code   | Name      | Description                                        |
|--------|-----------|----------------------------------------------------|
| `0x00` | Core      | Firmware version, uptime, memory                   |
| `0x01` | Radio     | TX/RX counts, CRC fails, channel utilization       |
| `0x02` | Packets   | Packet queue depth, routing table size             |

---

## Appendix A: Quick Reference — Payload Type Decoding Matrix

| Header bits [5:2] | Type         | Structure                                                            | Min size |
|--------------------|--------------|----------------------------------------------------------------------|----------|
| `0000` (0x00)      | REQ          | `[dest:1][src:1][MAC:2][enc...]`                                     | 4        |
| `0001` (0x01)      | RESPONSE     | `[dest:1][src:1][MAC:2][enc...]`                                     | 4        |
| `0010` (0x02)      | TXT_MSG      | unenc: `[ts:4][type:1][text...]` / enc: `[dest:1][src:1][MAC:2][enc...]` | 4   |
| `0011` (0x03)      | ACK          | `[checksum:4]`                                                       | 4        |
| `0100` (0x04)      | ADVERT       | `[pubkey:32][ts:4][sig:64][appData...]`                              | 100      |
| `0101` (0x05)      | GRP_TXT      | `[ch:1][MAC:2][enc...]`                                              | 3        |
| `0110` (0x06)      | GRP_DATA     | `[ch:1][MAC:2][enc...]`                                              | 3        |
| `0111` (0x07)      | ANON_REQ     | `[dest:1][epub:32][MAC:2][enc...]`                                   | 36       |
| `1000` (0x08)      | PATH         | `[dest:1][src:1][MAC:2][enc...]`                                     | 4        |
| `1001` (0x09)      | TRACE        | `[payload...]`                                                       | 0        |
| `1010` (0x0A)      | MULTIPART    | `[payload...]`                                                       | 0        |
| `1011` (0x0B)      | CONTROL      | `[subType:4b|flags:4b][data...]`                                     | 1        |
| `1111` (0x0F)      | RAW_CUSTOM   | `[customData...]`                                                    | 0        |

## Appendix B: ADVERT AppData Decoding Flow

```
1. Read flags byte
2. type = flags & 0x0F  -> NONE/CHAT/REPEATER/ROOM/SENSOR
3. if (flags & 0x10): read lat(4B int32 LE), lon(4B int32 LE)
4. if (flags & 0x20): read feat1(2B uint16 LE)
5. if (flags & 0x40): read feat2(2B uint16 LE)
6. if (flags & 0x80): read name(UTF-8 string, remaining bytes)
```

## Appendix C: Complete Frame Walkthrough

Example: Flood TXT_MSG with 1-byte hashes, 0 hops, unencrypted payload "Hi"

```
Header:    0x09  = 0b00_0010_01  -> version=0(v1), payloadType=0x02(TXT_MSG), routeType=0x01(Flood)
Path Len:  0x00  = 0b00_000000   -> hashSize=1B, hopCount=0
Payload:   [ts:4B LE][type:1B]["Hi"]
```

Total: `1 (header) + 1 (pathLen) + 0 (path) + 4 (ts) + 1 (txtType) + 2 (text) = 9 bytes` over the air.
