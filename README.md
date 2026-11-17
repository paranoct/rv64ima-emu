# rv64ima-emu

> 🇷🇺 [Русская версия](README_RU.md)

A single-file RV64IMA system emulator written in C++20. Boots MIT xv6-riscv and passes the full `usertests` suite.

## Overview

`rv64ima-emu` is a zero-dependency RISC-V emulator that implements the **RV64IMA** instruction set architecture with **Sv39** virtual memory, privilege modes (M/S/U), and a set of emulated peripherals sufficient to boot and interactively run a real UNIX-like operating system.

The entire emulator — CPU, MMU, bus, interrupt controller, block device, and UART — fits in a single `main.cpp` file (~2500 lines).

## Features

### CPU
- **RV64I** base integer instruction set (64-bit)
- **M extension** — hardware multiply/divide (`MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU` and their `*W` 32-bit variants)
- **A extension** — atomic operations (`LR.W/D`, `SC.W/D`, `AMOSWAP`, `AMOADD`, `AMOXOR`, `AMOAND`, `AMOOR`, `AMOMIN`, `AMOMAX`, `AMOMINU`, `AMOMAXU`)
- **Zicsr** — CSR access instructions (`CSRRW`, `CSRRS`, `CSRRC`, `CSRRWI`, `CSRRSI`, `CSRRCI`)
- **Zifencei** — instruction-fetch fence (`FENCE.I`)
- Three privilege modes: **Machine (M)**, **Supervisor (S)**, **User (U)**
- Trap delegation (`medeleg` / `mideleg`), `ECALL`, `EBREAK`, `MRET`, `SRET`, `WFI`

### Memory Management
- **Sv39** three-level page table translation
- 4 KB pages, 2 MB megapages, 1 GB gigapages
- Page fault generation (`InstructionPageFault`, `LoadPageFault`, `StorePageFault`) with `stval` recording
- `SFENCE.VMA` support

### Peripherals
| Device | Address Range | Description |
|--------|--------------|-------------|
| **UART 16550** | `0x10000000` | Serial console with RX/TX, LSR, IER, ISR registers. Keyboard input via a dedicated thread |
| **CLINT** | `0x02000000` | Core Local Interruptor — `mtime` / `mtimecmp` timer |
| **PLIC** | `0x0C000000` | Platform-Level Interrupt Controller — priority, enable, threshold, claim/complete |
| **VirtIO-blk** | `0x10001000` | VirtIO MMIO block device (v2). Processes descriptor chains directly from guest RAM via DMA |

### CSR Support
`mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`, `mscratch`, `mie`, `mip`, `medeleg`, `mideleg`, `mhartid`, `mcounteren`, `menvcfg`, `pmpcfg*`, `pmpaddr*`, `sstatus`, `stvec`, `sepc`, `scause`, `stval`, `sscratch`, `sie`, `sip`, `stimecmp`, `satp`, `time`

## Building

**Requirements:** C++20 compiler (Clang 14+ or GCC 12+). No external libraries required.

```bash
clang++ -std=c++20 -O3 main.cpp -o risc-v
```

## Running

1. Clone and build the xv6 kernel and filesystem image:
```bash
git clone https://github.com/mit-pdos/xv6-riscv.git
cd xv6-riscv
# Set -march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax in the Makefile
make kernel/kernel fs.img
cd ..
```

2. Run the emulator:
```bash
./risc-v
```

Expected output:
```
xv6 kernel is booting

$ ls
.              1 1 1024
..             1 1 1024
README         2 2 2227
cat            2 3 32856
echo           2 4 31720
...
$ usertests
usertests starting
test copyin: OK
test copyout: OK
...
ALL TESTS PASSED
$
```

The full `usertests` log is available in [`docs/usertests.log`](docs/usertests.log).

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                      RV64I CPU                       │
│  ┌────────┐ ┌──────┐ ┌──────────┐ ┌───────────────┐ │
│  │ x0–x31 │ │ CSRs │ │ Sv39 MMU │ │ Trap/IRQ Unit │ │
│  └────────┘ └──────┘ └──────────┘ └───────────────┘ │
└───────────────────────┬──────────────────────────────┘
                        │ Physical Address Bus
┌───────────────────────┴──────────────────────────────┐
│                        Bus                           │
│  ┌─────┐  ┌───────┐  ┌──────┐  ┌──────┐  ┌───────┐ │
│  │ RAM │  │ CLINT │  │ PLIC │  │ UART │  │VirtIO │ │
│  │128MB│  │ Timer │  │ IRQs │  │16550 │  │  blk  │ │
│  └─────┘  └───────┘  └──────┘  └──────┘  └───────┘ │
└──────────────────────────────────────────────────────┘
```

## Implementation Notes

- The emulator executes one instruction per `Step()` call (interpreter, no JIT). Each step increments `mtime` and checks pending interrupts before instruction fetch.
- VirtIO block device processes descriptor chains synchronously on `QUEUE_NOTIFY` write. Disk I/O is backed by a flat binary file (`fs.img`).
- UART input is handled by a detached `std::thread` that reads from `stdin` and pushes bytes into an RX buffer, raising PLIC IRQ 10.
- The PLIC implements priority-based arbitration with per-context enable masks, threshold filtering, and claim/complete semantics per the RISC-V PLIC specification.
- 128 MB of guest RAM is allocated at `0x80000000` (matching xv6 `KERNBASE`–`PHYSTOP`).

## References

- [The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA](https://riscv.org/specifications/)
- [The RISC-V Instruction Set Manual, Volume II: Privileged Architecture](https://riscv.org/specifications/privileged-isa/)
- [MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv)
- [Virtual I/O Device (VIRTIO) Specification v1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf)

## License

This project is provided as-is for educational and research purposes.
