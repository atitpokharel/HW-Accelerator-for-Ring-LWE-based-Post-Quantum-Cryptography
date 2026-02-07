# Ring-LWE hardware accelerator on DE1-SoC (N=16)

This repository is a small hardware-software co-design prototype for Ring-LWE style arithmetic on the DE1-SoC (Cyclone V plus dual-core ARM Cortex-A9).

It implements the main bottleneck, negacyclic polynomial multiplication in the ring Rq = Zq[x]/(x^N + 1), as an Avalon-MM memory-mapped accelerator. A bare-metal C program drives the accelerator to run a toy public key encryption flow (key generation, encryption, decryption) and measures cycle counts with the on-chip interval timer.

Parameters used in the prototype:
- Polynomial degree: N = 16
- Modulus: q = 257 (see sw/with_decryp.c)


## What the hardware does

Both RTL variants implement negacyclic convolution. For each multiply-accumulate term, the logic applies the x^N ≡ -1 rule on the fly:

- If k = i + j < N, accumulate + A[i] * S[j] into P[k]
- If k ≥ N, accumulate - A[i] * S[j] into P[k - N]

### Two architectures

1) Sequential core (ringlwe_mult16_mmio_flat)
- One multiplier.
- Nested-loop style scheduling.
- Latency is roughly N^2 cycles (plus FSM overhead).

2) Parallel core (ringlwe_mult16_mmio_parallel)
- 16 multipliers.
- Inner loop is unrolled in hardware.
- Latency is roughly N outer-loop steps (plus FSM overhead).

## Avalon-MM register map (word offsets)

The accelerator exposes a simple memory mapped register file.

| Offset | Name | R or W | Width | Notes |
|---:|---|---|---:|---|
| 0..15 | A[i] | R/W | 16 | Input polynomial A coefficients |
| 16..31 | S[i] | R/W | 16 | Input polynomial S coefficients |
| 32 | CTRL/STATUS | R/W | 32 | Write bit0=1 to start, read bit0=busy, bit1=done |
| 33..48 | P[i] | R | 32 | Output polynomial coefficients |

The C driver assumes the accelerator base is:
- 0xFF200000 + 0x00000200 = 0xFF200200

Adjust RING_LWE_BASE if your Platform Designer address map differs.

## Software

sw/with_decryp.c does the following:

- Writes input polynomials into the accelerator registers.
- Starts the FSM by writing 0x1 to CTRL/STATUS (offset 32).
- Polls until done is asserted (status & 0x2).
- Reads P[0..15].
- Uses the 100 MHz interval timer at 0xFF202000 to measure cycles.
- Runs a toy Ring-LWE style flow:
  - Key generation: p = A*s + e (mod q)
  - Encryption: u = A*r + e1 (mod q), v = p*r + e2 + m*(q/2) (mod q)
  - Decryption: m' = v - s*u (mod q), then threshold rounding

## Performance summary (from the report)

All cycle counts are measured with the 100 MHz interval timer (10 ns per cycle).

N=16, sequential accelerator

| Operation | SW cycles | HW cycles (compute only) | HW cycles (total, with comm) |
|---|---:|---:|---:|
| Key generation | 11,380 | 282 | 1,714 |
| Encryption | 22,753 | 542 | 3,034 |
| Decryption | 11,406 | 262 | 1,603 |

N=16, parallel accelerator

| Operation | SW cycles | HW cycles (compute only) | HW cycles (total, with comm) |
|---|---:|---:|---:|
| Key generation | 11,468 | 58 | 1,400 |
| Encryption | 22,892 | 116 | 2,725 |
| Decryption | 11,430 | 58 | 1,303 |

The big story: compute speedups are huge, but total speedups are capped by HPS to FPGA communication latency on this platform.

## Build and run (high level)

This repo focuses on the accelerator RTL and the bare-metal driver logic. Platform Designer and Quartus project files are not included.

Typical flow on DE1-SoC:
1. Create a Quartus project, then open Platform Designer.
2. Add one of the RTL modules as an Avalon-MM slave.
3. Map it into the LW-H2F bridge address space.
4. Ensure the interval timer is present and mapped as expected (or update TIMER_MM).
5. Compile and program the FPGA.
6. Build and run the bare-metal C program on the HPS.

## Notes and limitations

- This is a teaching scale prototype, not a production Kyber implementation.
- Parameters are intentionally small (N=16) to keep the hardware simple.
- The driver uses fixed test vectors and a simple rounding rule for message recovery.

## Citation

If you use this repo in a report or class submission, cite the included technical report in docs/Project_report.pdf.
