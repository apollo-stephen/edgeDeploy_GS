# SoftAP Configuration Persistence Design

## Problem

Running `idf.py set-target esp32s3` regenerated the ignored `sdkconfig` from
project defaults. The generated firmware therefore changed its SoftAP SSID
from `ESP32S3-CAPTURE` to the component's generic default, `myssid`. The
password and maximum station count were reset at the same time.

## Chosen approach

Persist the project-specific SoftAP values in `sdkconfig.defaults`:

- SSID: `ESP32S3-CAPTURE`
- password: restore the prior project value
- channel: 1
- maximum station count: 1

Keep `components/WIFIAP/Kconfig` generic and continue ignoring the generated
`sdkconfig`. This makes `set-target`, `fullclean`, and a fresh checkout produce
the intended project firmware without committing the full generated config.

## Alternatives considered

1. Change the defaults in `components/WIFIAP/Kconfig`. This would make a
   reusable component carry board/project policy, so it is not preferred.
2. Commit the complete `sdkconfig`. This would preserve the values but add a
   large generated file and create noisy ESP-IDF-version diffs.

## Documentation and tests

Add a repository test that reads `sdkconfig.defaults` and requires all four
SoftAP values. Update the README so the documented network name matches fresh
firmware. The test must fail before the defaults are changed and pass after.

## Verification

1. Run the focused configuration test, then the complete Python test suite.
2. Regenerate `sdkconfig` from `sdkconfig.defaults` and verify the effective
   values.
3. Rebuild the ESP32-S3 firmware and verify `edgeDeploy_GS.bin` contains the
   restored SSID and fits the application partition.
4. Hardware flashing and visibility from a computer remain a final device
   acceptance check.
