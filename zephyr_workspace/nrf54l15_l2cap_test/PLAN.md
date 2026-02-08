# L2CAP CoC Throughput Test - Plan

## Status: Implementation Complete

## Steps

1. **Project structure and config** - DONE
   - CMakeLists.txt, prj.conf, overlay

2. **Firmware (main.c)** - DONE
   - L2CAP server with dynamic PSM
   - PSM discovery GATT service
   - Semaphore-paced TX streaming
   - Connection/PHY/DLE callbacks
   - Stats thread

3. **Python test script** - DONE
   - PyObjC CoreBluetooth (not bleak)
   - GATT PSM discovery → L2CAP channel open
   - NSStreamDelegate for RX
   - Stats reporting

4. **Documentation** - DONE
   - README.md, context.md, PLAN.md

5. **Test and optimize** - TODO
   - Build and flash firmware
   - Run Python script
   - Compare throughput vs GATT baseline (~540 kbps)
   - Target: 700-900+ kbps

## Risks

- macOS may require encryption for L2CAP CoC → bump `sec_level` to `BT_SECURITY_L2`
- PyObjC NSStreamDelegate may need polling fallback
- Buffer exhaustion → mitigated by semaphore pacing (max 2 in-flight)
