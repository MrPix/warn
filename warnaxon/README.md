# Warnaxon

ESP32 classic CAN bus discovery, sniffing, and transmission over an SN65HVD230
transceiver. Discovery and sniffing use listen-only mode; transmission uses
normal TWAI mode and therefore participates in CAN acknowledgements and errors.

## Wiring

The firmware pin assignment is:

| ESP32 | SN65HVD230 module | Purpose |
|---|---|---|
| 3V3 | VCC / 3V3 | Transceiver power |
| GND | GND | Common ground |
| GPIO21 | D / TXD / CTX | ESP32 CAN transmit signal |
| GPIO22 | R / RXD / CRX | ESP32 CAN receive signal |
| - | CANH | CAN bus high |
| - | CANL | CAN bus low |

Signal direction:

```text
ESP32 GPIO21 (TWAI TX)  ---->  D/TXD  SN65HVD230
ESP32 GPIO22 (TWAI RX)  <----  R/RXD  SN65HVD230

                                    CANH ---- CAN bus CANH
                                    CANL ---- CAN bus CANL

ESP32 GND               -------     GND  ---- CAN bus ground
ESP32 3V3               -------     VCC
```

Use a 3.3 V SN65HVD230 module. Do not power its logic side from 5 V. Connect
the ESP32, transceiver, and CAN network grounds together. If the module exposes
an `RS` pin, connect it low for normal high-speed operation unless the module
already provides the required resistor.

## Bus Termination

A CAN bus should have one 120 ohm termination resistor at each physical end of
the trunk. With bus power removed, resistance measured between CANH and CANL
should normally be about 60 ohms because the two terminators are in parallel.

Do not add another termination resistor when connecting the sniffer to an
already correctly terminated bus. Keep the CANH/CANL stub to the sniffer short.

When Warnaxon is sniffing, it operates in listen-only mode, so another active
CAN node must acknowledge transmitted frames. A test setup containing only one
transmitter and this sniffer may produce repeated transmissions or CAN errors
because the sniffer deliberately does not send acknowledgements. The
`can send` command uses normal mode and can transmit acknowledgements.

## Commands

Search the standard classic CAN bitrates:

```text
can search [--start <kbps>] [--end <kbps>] [--time <seconds>]
```

The defaults are 10 through 1000 kbps and 20 seconds at each rate. Supported
rates are 10, 20, 50, 100, 125, 250, 500, 800, and 1000 kbps. Start and end are
inclusive; reversing them scans downward. Search reports and skips rates that
the ESP32 TWAI clock cannot generate. In particular, 10 kbps is not achievable
on ESP32, and 20 kbps requires a sufficiently recent ESP32 chip revision.

Examples:

```text
can search
can search --start 125 --end 1000 --time 5
can search --start 500 --end 125 --time 2
```

Sniff continuously at one bitrate:

```text
can sniff --bitrate 500
```

Press lowercase `c` to stop sniffing. Each frame includes the bitrate, the
TWAI hardware timestamp in microseconds, the time since the previous frame,
the standard or extended identifier, DLC, RTR state, and payload bytes.

Configure a persistent transmit bitrate and send a standard frame:

```text
can setbitrate 500
can send 0x123 DE AD BE EF
```

The transmit command accepts a standard 11-bit hexadecimal ID and zero to eight
hexadecimal data bytes. Run `can setbitrate` again to change the transmit
bitrate. Running `can search` or `can sniff` stops the transmitter before
entering listen-only mode.

## Build And Flash

Run from an ESP-IDF terminal:

```powershell
idf.py build
idf.py flash monitor
```

The console runs on UART0 at 115200 baud. Type `help` to list commands.