# Restart into bring-up mode sometimes came back in normal mode (2026-11-25)

## Question

On 2026-11-25, on the bench WT32-ETH01 (ESP32, ESP-IDF v5.5.6, esp32-WoLviaBT 2cfaf12 with
esp-bringup d4f3f1d, netserial log hook active, on Wi-Fi), why did
`POST /api/setup/action?do=bringup` answer "restarting into bring-up mode" but the device came back
in normal mode (`bringup:false`, no hotspot)? The same call worked once at 18:55 UTC, then failed
twice in a row at 19:05 and 19:07.

## Answer

The restart task (`bu_restart`) had a 2048-byte stack. `bringup_enter()` logs (through netserial's
vprintf hook, which runs on the caller's stack) and writes NVS on that stack. It overflowed, so the
device rebooted by panic before the `_force` flag was written. Raising the stack to 4096 fixed it:
three tries out of three reached bring-up mode.

## Procedure

1. Read the netserial log around each reboot. It showed no "entering bring-up mode on request"
   line before the failing reboots, so `bringup_enter()` had not logged anything.
2. Checked that NVS was not full or failing. Added an NVS stats line and an error log to the
   flag write. Result: 183 entries used, 321 free, and no write error was logged.
3. Logged `esp_reset_reason()` at boot and reproduced the failure on the unchanged 2048-byte stack.
   The OTA reboot gives reset reason 3 (SW), which is the control. The `do=bringup` reboot gave
   4 (PANIC).
4. Changed only the stack size to 4096. Tried three times in a row, with a `do=reboot` between
   tries. All three came back in bring-up mode ("requested", hotspot on). The flag was confirmed
   set in the log before the reset.

## Evidence

```
19:08:4x I (671) bringup: reset reason 3     <- OTA reboot (control)
19:08:5x I (633) bringup: reset reason 4     <- do=bringup, stack 2048: PANIC
19:10:0x I (13631) bringup: force flag now 1 <- do=bringup, stack 4096
19:10:1x I (633) bringup: reset reason 3     <- SW reset, flag survived
try 1: True requested True
try 2: True requested True
try 3: True requested True
```

## Measured and REJECTED (do not retry)

- NVS full or the flag write failing: ruled out. 321 entries were free and no write error was
  logged.

## Measurement traps

- The panic backtrace goes to UART0 only, which is not connected on the bench. netserial loses
  everything after the panic. That is why the only evidence of the crash was a reset reason
  logged on the next boot.
- `do=reboot` uses the same task but never logs before `esp_restart()`, so it never overflowed and
  hid the problem.

## Open

- Why the 18:55 attempt worked with the same 2048-byte stack was not determined. The likely
  cause is that stack use depends on how much netserial's hook does per line, for example
  whether a client is connected.
