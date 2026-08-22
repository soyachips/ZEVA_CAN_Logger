# ZEVA ESP32 Cell Voltage Logger

This project is a minimal ESP32-based logger for ZEVA BMS CAN data. The goal is to listen for ZEVA BMS packets on CAN and print individual cell voltages to the serial terminal.

## Current behavior
- uses ESP32 TWAI (native CAN) on pins `TX=21`, `RX=22`
- listens for 29-bit ZEVA BMS packet IDs starting at `0x12C` (decimal 300)
- collects the 4 BMS reply packets for each module
- merges module data into a single pack row and prints a complete sequential list of active cells
- SD logging is disabled until you are ready to wire and test the SD module

## Required hardware
- ESP32-WROOM-32 module
- CAN transceiver module (for example MCP2562 or SN65HVD230)
- 120Ω termination resistor on each end of the CAN bus
- optional SD card module later for logging to file

## Suggested wiring
### CAN transceiver
- ESP32 `GND` → CAN transceiver `GND`
- ESP32 `3.3V` → CAN transceiver `VCC` (or `5V` if the board requires it)
- ESP32 `GPIO21` → CAN transceiver `TX` / `TXD`
- ESP32 `GPIO22` → CAN transceiver `RX` / `RXD`
- CAN transceiver `CANH` and `CANL` → CAN bus lines
- Terminate the bus with 120Ω at both ends

### SD card module (not enabled yet)
- ESP32 `3.3V` → SD `3V3`
- ESP32 `GND` → SD `GND`
- ESP32 `GPIO23` → SD `MOSI`
- ESP32 `GPIO19` → SD `MISO`
- ESP32 `GPIO18` → SD `CLK` / `SCK`
- ESP32 `GPIO15` → SD `CS`

> Use the SD module’s 3.3V supply pin, not 5V, since the ESP32 is 3.3V logic.

## Serial output format
The firmware prints lines like:

```
12345,packV=96.5,packI=0.6,cell1=3.712,cell2=3.705,cell3=3.710,...,cells=26
```

- `timestamp_ms`: milliseconds since startup
- `packV`: pack voltage in volts with one decimal place (raw ZEVA value is in tenths of volts)
- `packI`: pack current in amps with one decimal place (raw ZEVA current is a signed 24-bit value offset by 8,388,608 and converted to 0.1 A units)
- `cellN`: individual cell voltage in volts with three decimal places (raw ZEVA cell voltage is in millivolts)
- `cells`: total number of printed cell values in the row

## Notes
- The current code merges all active BMS module cells into a single row instead of printing one row per module.
- If ZEVA `CORE_SEND_CELL_NUMS` is received, the logger uses the protocol-provided module cell counts.
- Once you are ready to enable SD logging, the code can be extended to store CSV rows on the card.

## To Do
- Replace CAN transceiver with 3.3V alternative
- Wire up MicroSD module
- Save data as CSV