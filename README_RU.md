# rv64ima-emu

> 🇬🇧 [English version](README.md)

Однофайловый системный эмулятор RV64IMA на C++20. Загружает MIT xv6-riscv и полностью проходит набор тестов `usertests`.

## Обзор

`rv64ima-emu` — эмулятор процессора RISC-V без внешних зависимостей, реализующий архитектуру **RV64IMA** с виртуальной памятью **Sv39**, уровнями привилегий (M/S/U) и набором периферийных устройств, достаточным для загрузки и интерактивной работы настоящей UNIX-подобной операционной системы.

Весь эмулятор — процессор, MMU, шина, контроллер прерываний, блочное устройство и UART — уместился в одном файле `main.cpp` (~2500 строк).

## Возможности

### Процессор
- **RV64I** — базовый набор целочисленных инструкций (64-бит)
- **Расширение M** — аппаратное умножение и деление (`MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU` и 32-битные варианты `*W`)
- **Расширение A** — атомарные операции (`LR.W/D`, `SC.W/D`, `AMOSWAP`, `AMOADD`, `AMOXOR`, `AMOAND`, `AMOOR`, `AMOMIN`, `AMOMAX`, `AMOMINU`, `AMOMAXU`)
- **Zicsr** — инструкции доступа к CSR (`CSRRW`, `CSRRS`, `CSRRC`, `CSRRWI`, `CSRRSI`, `CSRRCI`)
- **Zifencei** — барьер выборки инструкций (`FENCE.I`)
- Три уровня привилегий: **Machine (M)**, **Supervisor (S)**, **User (U)**
- Делегирование исключений (`medeleg` / `mideleg`), `ECALL`, `EBREAK`, `MRET`, `SRET`, `WFI`

### Управление памятью
- Трёхуровневая трансляция адресов **Sv39**
- Страницы 4 КБ, мегастраницы 2 МБ, гигастраницы 1 ГБ
- Генерация page fault (`InstructionPageFault`, `LoadPageFault`, `StorePageFault`) с записью адреса в `stval`
- Поддержка `SFENCE.VMA`

### Периферия
| Устройство | Диапазон адресов | Описание |
|------------|-----------------|----------|
| **UART 16550** | `0x10000000` | Последовательная консоль с регистрами RX/TX, LSR, IER, ISR. Ввод с клавиатуры через отдельный поток |
| **CLINT** | `0x02000000` | Локальный контроллер прерываний — таймер `mtime` / `mtimecmp` |
| **PLIC** | `0x0C000000` | Контроллер внешних прерываний — приоритет, маски, порог, claim/complete |
| **VirtIO-blk** | `0x10001000` | Блочное устройство VirtIO MMIO (v2). Обработка цепочек дескрипторов напрямую из гостевой RAM через DMA |

### Поддерживаемые CSR
`mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`, `mscratch`, `mie`, `mip`, `medeleg`, `mideleg`, `mhartid`, `mcounteren`, `menvcfg`, `pmpcfg*`, `pmpaddr*`, `sstatus`, `stvec`, `sepc`, `scause`, `stval`, `sscratch`, `sie`, `sip`, `stimecmp`, `satp`, `time`

## Сборка

**Требования:** компилятор C++20 (Clang 14+ или GCC 12+). Внешние библиотеки не требуются.

```bash
clang++ -std=c++20 -O3 main.cpp -o risc-v
```

## Запуск

1. Клонировать и собрать ядро xv6 и образ файловой системы:
```bash
git clone https://github.com/mit-pdos/xv6-riscv.git
cd xv6-riscv
# Установить -march=rv64ima_zicsr_zifencei -mabi=lp64 -mno-relax в Makefile
make kernel/kernel fs.img
cd ..
```

2. Запустить эмулятор:
```bash
./risc-v
```

Ожидаемый вывод:
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

Полный лог `usertests` доступен в [`docs/usertests.log`](docs/usertests.log).

## Архитектура

```
┌──────────────────────────────────────────────────────┐
│                    Процессор RV64I                   │
│  ┌────────┐ ┌──────┐ ┌──────────┐ ┌───────────────┐  │
│  │ x0–x31 │ │ CSR  │ │ MMU Sv39 │ │ Прерывания    │  │
│  └────────┘ └──────┘ └──────────┘ └───────────────┘  │
└───────────────────────┬──────────────────────────────┘
                        │ Шина физических адресов
┌───────────────────────┴──────────────────────────────┐
│                       Шина (Bus)                     │
│  ┌─────┐  ┌───────┐  ┌──────┐  ┌──────┐  ┌───────┐   │
│  │ RAM │  │ CLINT │  │ PLIC │  │ UART │  │VirtIO │   │
│  │128МБ│  │Таймер │  │  IRQ │  │16550 │  │  blk  │   │
│  └─────┘  └───────┘  └──────┘  └──────┘  └───────┘   │
└──────────────────────────────────────────────────────┘
```

## Детали реализации

- Эмулятор выполняет по одной инструкции за вызов `Step()` (интерпретатор, без JIT). На каждом шаге инкрементируется `mtime` и проверяются ожидающие прерывания до выборки инструкции.
- Блочное устройство VirtIO обрабатывает цепочки дескрипторов синхронно при записи в `QUEUE_NOTIFY`. Дисковый ввод-вывод опирается на плоский бинарный файл (`fs.img`).
- Ввод через UART обслуживается отдельным `std::thread`, который читает `stdin` и помещает байты в буфер RX, генерируя прерывание PLIC IRQ 10.
- PLIC реализует арбитраж на основе приоритетов с масками разрешений по контексту, фильтрацией по порогу и семантикой claim/complete в соответствии со спецификацией RISC-V PLIC.
- Гостевая RAM: 128 МБ, начиная с адреса `0x80000000` (соответствует `KERNBASE`–`PHYSTOP` в xv6).

## Ссылки

- [The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA](https://riscv.org/specifications/)
- [The RISC-V Instruction Set Manual, Volume II: Privileged Architecture](https://riscv.org/specifications/privileged-isa/)
- [MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv)
- [Virtual I/O Device (VIRTIO) Specification v1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf)

## Лицензия

Проект предоставляется как есть в образовательных и исследовательских целях.
