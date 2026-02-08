# L2CAP CoC Throughput Test - Plan

## Status: Step 5 - Testing & Optimization (In Progress)

## Steps

1. **Project structure and config** - DONE
   - CMakeLists.txt, prj.conf, overlay

2. **Firmware (main.c)** - DONE
   - L2CAP server with dynamic PSM
   - PSM discovery GATT service
   - Semaphore-paced TX streaming with alloc_buf for RX segmentation
   - Connection/PHY/DLE callbacks with DLE-aware streaming gate
   - Stats thread

3. **Python test script** - DONE
   - PyObjC CoreBluetooth (not bleak)
   - GATT PSM discovery → L2CAP channel open
   - NSStreamDelegate for RX (with PyObjC tuple return fix)
   - Steady-state + average stats reporting

4. **Documentation** - DONE
   - README.md, context.md, PLAN.md, PROMPT.md

5. **Test and optimize** - IN PROGRESS
   - Build and flash firmware ✓
   - Run Python script ✓
   - Compare throughput vs GATT baseline (~540 kbps)
   - **Current L2CAP steady-state: ~517 kbps** (matching GATT)
   - Target: ≥650 kbps

## Optimization History

| Iteration | SDU Size | Flow Control | Buffers | Steady-State | Notes |
|-----------|----------|-------------|---------|-------------|-------|
| 1 | 2000 | sem, 1 give | 2 tx | FAIL (-122) | SDU > tx.mtu, no alloc_buf |
| 2 | 2000→1251 | sem, 1 give | 2 tx | 200 kbps | alloc_buf added, dynamic SDU |
| 3 | 1251 | sem, 2 give | 4 tx | 440 kbps | Double-buffer, more frags |
| 4 | 1251 | sem, 6 give | 6 tx | 440 kbps | More buffers didn't help |
| 5 | 495 | sem, 6 give | 6 tx | **540 kbps** | Less fragmentation overhead |
| 6 | 244 | sem, 6 give | 6 tx | 520 kbps | More SDU overhead per packet |
| 7 | 495 | sleep(5ms) | 6 tx | 490 kbps | Sleep-based worse than sem |
| 8 | 741 | sem, 6 give | 6 tx | 520 kbps | 3-packet SDU less optimal |
| 9 | 495 | sem, 3 give | 3 tx | 517 kbps | DLE gate + adv stop + stats |

## Key Findings

1. **alloc_buf callback required**: Without it, rx.mtu truncated to 245 (BT_L2CAP_SDU_RX_MTU)
2. **SDU size must ≤ tx.mtu**: macOS negotiates tx.mtu=1251, sending larger causes ENOSPC
3. **495-byte SDU is optimal**: Best balance of fragmentation overhead vs per-SDU overhead
4. **Semaphore flow control > sleep-based**: Credit-based pacing works better than timed sends
5. **Stats thread helps**: The printk yield provides beneficial CPU time sharing
6. **DLE ramp-up wastes first seconds**: Gating on dle_ready eliminates 27-byte packet waste
7. **CI ramp-up is the bigger delay**: macOS starts at ~30ms CI, takes ~5s to reach 15ms
8. **Both GATT and L2CAP hit ~540 kbps**: Same physical layer ceiling

## Bottleneck Analysis

- **Theoretical max** at 15ms CI, 2M PHY, 251-byte DLE: ~1.3 Mbps
- **Actual**: ~520 kbps = ~40% of theoretical
- **Root cause**: Zephyr open-source LL controller (BT_LL_SW_SPLIT) doesn't efficiently pack packets into connection events
- **Solution**: Nordic SoftDevice Controller (SDC) via nRF Connect SDK (not currently installed)

## Next Steps

1. Install nRF Connect SDK and switch to SoftDevice Controller (SDC)
2. Or test with a Linux central (BlueZ) which may accept shorter CI
3. Or test with iOS device which may negotiate different parameters
4. Explore CONFIG_BT_CTLR_ADVANCED_FEATURES for Zephyr LL tuning
