# hidws - WebSocket/USB HID gateway

`hidws` is a lightweight C daemon that bridges WebSocket clients to USB HID
devices, letting web apps talk to HID devices remotely over the network
without direct USB access.

It also ships `hid-list`, a small diagnostic tool that lists all USB HID
devices and probes the reports they support.

Both programs link against the hidapi **libusb** backend and do **not**
require kernel HID support (`/dev/hidraw*`), so they work even on systems
without a HID/INPUT kernel subsystem.

## Prerequisites (Ubuntu/Debian)

```bash
sudo apt install libhidapi-dev libwebsockets-dev
```

The programs link with `-lhidapi-libusb` at build time. `libhidapi-dev` must
ship the precompiled `libhidapi-libusb.so` backend (usbfs, libusb).

## USB device permissions

To read/write HID devices as a regular user:

### 1. Add user to `plugdev` group

```bash
sudo usermod -aG plugdev $USER
```

Log out and back in for the change to take effect.

### 2. Udev rules

Create `/etc/udev/rules.d/99-hid.rules`:

```
# Permissions for HID and USB devices (covers both hidraw and libusb backends)
SUBSYSTEM=="hidraw", MODE="0666"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", MODE="0664", GROUP="plugdev"
```

Then reload:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Build & Install

```bash
make
sudo make install          # PREFIX=/usr/local
```

This builds both `hidws` and `hid-list`.

> **MIPS/uClibc note:** on the [Freetz-EVO](https://github.com/Ircama/freetz-evo/) MIPS toolchain (GCC 13.4.0, uClibc-ng, `-march=34kc -msoft-float`), compiling `hidws` with `-O1` or higher miscompiles the reader thread and crashes with a NULL deref inside `hid_read_timeout` (only `-O0` is stable there). On other platforms the default flags are fine. If you hit a `Segmentation fault` right after `[hid] Reader thread started`, rebuild with `-O0`.

## Usage

### hidws - WebSocket/HID bridge

```bash
./hidws [port] [--cert FILE] [--key FILE]
```

Default port is `9001`. Point your WebSocket client to `ws://<host>:9001`.
The HID device is released automatically when the client disconnects.

### TLS / WSS support (same port as WS)

Pass `--cert` (and optionally `--key`) to serve **both** plain `ws://` and
encrypted `wss://` on the **same port**:

```bash
./hidws 9001 --cert /etc/hidws/server.crt --key /etc/hidws/server.key
```

- `--key` is optional: if omitted, it is derived from the cert path by
  replacing the extension with `.key` (`server.crt` -> `server.key`).
- If the certificate file does not exist yet and hidws was built with SSL
  support, a **self-signed** certificate/key pair is generated automatically
  at first start (RSA-2048, valid 10 years, CN/SAN `fritz.box`).
- Both schemes are served by the same listening socket, so
  `ws://192.168.178.1:9001` **and** `wss://192.168.178.1:9001` work side by
  side. This is handy on HTTPS-hosted web apps (e.g. GitHub Pages) which block
  plain `ws://` connections: the app can fall back to `wss://`.

> Note: the self-signed certificate is not trusted by clients, so browsers
> must be told to accept it (a one-time warning) or the app must be configured
> to allow self-signed certificates.

### Diagnostic page (http/https on the same port)

Opening `http://<host>:<port>/` or `https://<host>:<port>/` in a browser
serves a small **diagnostic page** that confirms the daemon is reachable and
shows version, port and the available WebSocket endpoints, plus **"Test ws://"
/ "Test wss://"** buttons that open a real WebSocket to the server and report
success/failure.

This is the convenient way to confirm access and (for `wss://`) to accept the
browser's one-time security exception for the self-signed certificate: visit
`https://192.168.178.1:9001/`, accept the warning, then reload the page and
press "Test wss://".

### hid-list - enumerate HID devices

```bash
./hid-list
```

Prints every USB HID device (path, VID/PID, manufacturer, product, serial) and probes its feature and input reports.

## Wire Protocol

All messages are JSON over WebSocket.

### Client -> Server

| Command | Payload |
|---------|---------|
| `list` | `{"cmd":"list"}` |
| `open` | `{"cmd":"open","vendorId":<int>,"productId":<int>}` |
| `send_report` | `{"cmd":"send_report","reportId":<int>,"data":[<bytes>]}` |
| `send_feature_report` | `{"cmd":"send_feature_report","reportId":<int>,"data":[<bytes>]}` |
| `close` | `{"cmd":"close"}` |

### Server -> Client

| Type | Payload |
|------|---------|
| `device_list` | `{"type":"device_list","devices":[{"vendorId":...,"productId":...,"productName":"..."}]}` |
| `opened` | `{"type":"opened","vendorId":...,"productId":...,"productName":"...","usagePage":...,"usage":...,"interfaceNumber":...,"busType":...,"releaseNumber":...,"serialNumber":"...","reportDescriptor":[...]}` |
| `input_report` | `{"type":"input_report","reportId":<int>,"data":[<bytes>]}` |
| `ok` | `{"type":"ok"}` |
| `error` | `{"type":"error","message":"..."}` |
| `closed` | `{"type":"closed"}` |

> `opened` also carries the hidapi device info (`usagePage`, `usage`, `interfaceNumber`, `busType`, `releaseNumber`, `serialNumber`) and the raw HID report descriptor as a byte array (`reportDescriptor`), so clients can render the same report collections (input/output/feature with report IDs and items) as a local WebHID connection.

## Notes

- The KT02H20 family (FiiO JA11 etc.) uses OUTPUT reports; frontends must send data with `send_report` (report ID 0x02), not feature reports.
- `input_report` data received over WebSocket includes the report-ID byte as the first element for numbered input reports; WebHID strips it, so remote frontends must strip it to match.

## License

European Union Public Licence 1.2 (EUPL-1.2). See [LICENSE](LICENSE).

