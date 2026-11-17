#include <mutex>
#include <array>
#include <deque>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

enum class TrapCause : uint64_t {
    InstructionAddressMisaligned = 0,
    InstructionAccessFault       = 1,
    IllegalInstruction           = 2,
    Breakpoint                   = 3,
    LoadAddressMisaligned        = 4,
    LoadAccessFault              = 5,
    StoreAddressMisaligned       = 6,
    StoreAccessFault             = 7,
    UserEnvCall                  = 8,
    SupervisorEnvCall            = 9,
    MachineEnvCall               = 11,
    InstructionPageFault         = 12,
    LoadPageFault                = 13,
    StorePageFault               = 15
};

enum class AccessType {
    Fetch,
    Load,
    Store
};

class Ram {
private:
    uint64_t base;
    std::vector<uint8_t> data;

public:
    Ram(uint64_t base, size_t size): base(base), data(size, 0) {};

    // does this addr contain to curr RAM
    bool Contains(uint64_t addr) const {
        return addr >= base && addr - base < data.size();
    }
    // ptr to byte (physical addr)
    uint8_t& At(uint64_t addr) { return data[addr - base]; }

    uint64_t GetRamBase() {
        return base;
    }

    size_t GetRamSize() {
        return data.size();
    }
};

class PLIC {
private:
    static constexpr uint32_t NUM_SOURCES  = 1024;
    static constexpr uint32_t NUM_WORDS    = NUM_SOURCES / 32;
    static constexpr uint32_t NUM_CONTEXTS = 2;
    static constexpr uint32_t S_CONTEXT    = 1;

    std::array<uint32_t, NUM_SOURCES> priority{};
    std::array<uint32_t, NUM_WORDS>   pending{};
    std::array<std::array<uint32_t, NUM_WORDS>, NUM_CONTEXTS> enable{};
    std::array<uint32_t, NUM_CONTEXTS> threshold{};
    mutable std::mutex mtx;

    uint32_t ClaimSUnlocked() {
        uint32_t bestIrq  = 0;
        uint32_t bestPrio = threshold[S_CONTEXT];
        for (uint32_t w = 0; w < NUM_WORDS; ++w) {
            uint32_t mask = pending[w] & enable[S_CONTEXT][w];
            while (mask) {
                uint32_t b   = __builtin_ctz(mask);
                uint32_t irq = w * 32 + b;
                if (priority[irq] > bestPrio) {
                    bestPrio = priority[irq];
                    bestIrq  = irq;
                }
                mask &= mask - 1;
            }
        }
        if (bestIrq) {
            pending[bestIrq / 32] &= ~(1u << (bestIrq % 32));
        }
        return bestIrq;
    }

public:
    void SetPending(uint32_t irq) {
        std::lock_guard<std::mutex> lk(mtx);
        if (irq == 0 || irq >= NUM_SOURCES) {
            return;
        }
        uint32_t word = irq / 32;
        uint32_t bit  = irq % 32;
        pending[word] |= (1u << bit);
    }

    bool PendingS() const {
        std::lock_guard<std::mutex> lk(mtx);
        for (uint32_t w = 0; w < NUM_WORDS; ++w) {
            uint32_t mask = pending[w] & enable[S_CONTEXT][w];
            while (mask) {
                uint32_t b   = __builtin_ctz(mask);
                uint32_t irq = w * 32 + b;
                if (priority[irq] > threshold[S_CONTEXT]) return true;
                mask &= mask - 1;
            }
        }
        return false;
    }

    uint32_t ClaimS() {
        std::lock_guard<std::mutex> lk(mtx);
        return ClaimSUnlocked();
    }

    bool Read(uint32_t offset, uint32_t& out) {
        std::lock_guard<std::mutex> lk(mtx);
        // priority
        if (offset < 0x1000) {
            uint32_t irq = offset / 4;
            out = (irq < NUM_SOURCES) ? priority[irq] : 0;
            return true;
        }
        // pending (read-only)
        if (offset >= 0x1000 && offset < 0x1080) {
            uint32_t w = (offset - 0x1000) / 4;
            out = (w < NUM_WORDS) ? pending[w] : 0;
            return true;
        }
        // enable
        if (offset >= 0x2000 && offset < 0x20000) {
            uint32_t ctx = (offset - 0x2000) / 0x80;
            uint32_t w   = ((offset - 0x2000) % 0x80) / 4;
            out = (ctx < NUM_CONTEXTS && w < NUM_WORDS) ? enable[ctx][w] : 0;
            return true;
        }
        // context: threshold / claim
        if (offset >= 0x200000 && offset < 0x200000 + NUM_CONTEXTS * 0x1000) {
            uint32_t ctx = (offset - 0x200000) / 0x1000;
            uint32_t reg = (offset - 0x200000) % 0x1000;
            if (reg == 0x000) { out = threshold[ctx]; return true; }
            if (reg == 0x004) {
                out = (ctx == S_CONTEXT) ? ClaimSUnlocked() : 0;
                return true;
            }
            out = 0;
            return true;
        }
        out = 0;
        return true;
    }

    bool Write(uint32_t offset, uint32_t value) {
        std::lock_guard<std::mutex> lk(mtx);
        // priority
        if (offset < 0x1000) {
            uint32_t irq = offset / 4;
            if (irq != 0 && irq < NUM_SOURCES) priority[irq] = value & 0x7;
            return true;
        }
        // pending read-only
        if (offset >= 0x1000 && offset < 0x1080) return true;
        // enable
        if (offset >= 0x2000 && offset < 0x20000) {
            uint32_t ctx = (offset - 0x2000) / 0x80;
            uint32_t w   = ((offset - 0x2000) % 0x80) / 4;
            if (ctx < NUM_CONTEXTS && w < NUM_WORDS) enable[ctx][w] = value;
            return true;
        }
        // context
        if (offset >= 0x200000 && offset < 0x200000 + NUM_CONTEXTS * 0x1000) {
            uint32_t ctx = (offset - 0x200000) / 0x1000;
            uint32_t reg = (offset - 0x200000) % 0x1000;
            if (reg == 0x000) { threshold[ctx] = value & 0x7; return true; }
            // reg == 0x004 — complete, для эмулятора no-op
            return true;
        }
        return true;
    }
};

class Bus;

class VirtIODevice {
private:
    uint32_t status = 0;
    uint64_t desc_addr = 0;
    uint64_t avail_addr = 0;
    uint64_t used_addr = 0;
    uint32_t device_features_sel = 0;
    uint32_t driver_features = 0;
    uint32_t queue_sel = 0;
    uint32_t queue_num = 0;
    uint32_t queue_ready = 0;
    uint32_t interrupt_status = 0;
    uint64_t last_avail_idx = 0;
    Bus* bus = nullptr;
    std::fstream disk;
    
public:
    void SetBus(Bus* bus) {
        this->bus = bus;
    }

    bool OpenDisk() {
        disk.open("xv6-riscv/fs.img", std::ios::binary | std::ios::in | std::ios::out);
        if (!disk.is_open()) { return false; }
        return true;
    }

    bool HandleNotify();

    bool Read(uint64_t offset, uint32_t& out) {
        switch(offset) {
        case 0x00: {
            out = 0x74726976;
            return true;
        }

        case 0x04: 
            out = 2;
            return true;

        case 0x08:
            out = 2;
            return true;

        case 0x10:
            out = 0;
            return true;
        
        case 0x14:
            out = device_features_sel;
            return true;

        case 0x30:
            out = queue_sel;
            return true;

        case 0x38:
            out = queue_num;
            return true;

        case 0x44:
            out = queue_ready;
            return true;

        case 0x60:
            out = interrupt_status;
            return true;

        case 0x0C:
            out = 0x554d4551;
            return true;

        case 0x34:
            out = 8;
            return true;

        case 0x70:
            out = status;
            return true;
        
        default:
            out = 0;
            return true;
        }
    }

    bool Write(uint64_t offset, uint32_t value) {
        switch(offset) {
        case 0x00:
        case 0x04: 
        case 0x08:
        case 0x0C:
        case 0x34:
        case 0x24:
            return true;

        case 0x20:
            driver_features = value;
            return true;
        
        case 0x30:
            queue_sel = value;
            return true;

        case 0x38:
            queue_num = value;
            return true;

        case 0x44:
            queue_ready = value;
            return true;
        
        case 0x50:
            HandleNotify();
            return true;

        case 0x64:
            interrupt_status &= ~value;
            return true;

        case 0x70:
            if (value == 0) {
                status = 0;
                desc_addr = 0;
                avail_addr = 0;
                used_addr = 0;
                device_features_sel = 0;
                driver_features = 0;
                queue_sel = 0;
                queue_num = 0;
                queue_ready = 0;
                interrupt_status = 0;
                last_avail_idx = 0;
                
            }
            else {
                status = value;
            }
            return true;

        case 0x80: {
            desc_addr = (desc_addr & 0xFFFFFFFF00000000ULL) | value;
            return true;
        }

        case 0x84: {
            desc_addr = (desc_addr & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
            return true;
        }

        case 0x90: {
            avail_addr = (avail_addr & 0xFFFFFFFF00000000ULL) | value;
            return true;
        }

        case 0x94: {
            avail_addr = (avail_addr & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
            return true;
        }

        case 0xA0: {
            used_addr = (used_addr & 0xFFFFFFFF00000000ULL) | value;
            return true;
        }

        case 0xA4: {
            used_addr = (used_addr & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
            return true;
        }

        default:
            return true;
        }
    }
};

class Bus {
private:
    Ram ram;
    uint64_t mtime = 0;
    uint64_t mtimecmp = 0;
    VirtIODevice virtio;
    PLIC plic;
    std::deque<uint8_t> rxBuf;
    uint8_t ier;
    std::mutex rxMtx;

public:
    Bus(uint64_t ramBase, size_t ramSize) : ram(ramBase, ramSize) {
        virtio.SetBus(this); virtio.OpenDisk();
    };

    void PushRx(uint8_t byte) {
        std::lock_guard<std::mutex> lk(rxMtx);
        rxBuf.push_back(byte);
        if (ier & 1) RaiseInterrupt(10);
    }

    void Tick() { mtime++; }

    bool IsTimerInterruptPending() const {
        return mtime >= mtimecmp;
    }

    void RaiseInterrupt(uint32_t irq) {
        plic.SetPending(irq);
    }

    bool IsExternalInterruptPending() const {
        return plic.PendingS();
    }

    uint64_t GetMtime() const { return mtime; }

    bool Read(uint64_t addr, size_t size, uint64_t& out) {
        if (addr >= 0x10000000 && addr < 0x10000008) {
            if (size != 1) return false;
            uint64_t offset = addr - 0x10000000;
            switch(offset) {
            case 0: {
                std::lock_guard<std::mutex> lk(rxMtx);
                if (rxBuf.empty()) {
                    out = 0;
                }
                else {
                    out = rxBuf.front();
                    rxBuf.pop_front();
                }
                return true;
            }

            case 1: {
                std::lock_guard<std::mutex> lk(rxMtx);
                out = ier;
                return true;
            }

            case 2: {
                std::lock_guard<std::mutex> lk(rxMtx);
                if (!rxBuf.empty() && (ier & 1)) {
                    out = 0x04; 
                } else if (ier & 2) {
                    out = 0x02; 
                } else {
                    out = 0x01; 
                }
                return true;
            }

            case 5: {
                std::lock_guard<std::mutex> lk(rxMtx);
                if (rxBuf.empty()) {
                    out = 0b00100000;
                }
                else {
                    out = 0b00100001;
                }
                return true;
            }

            default:
                out = 0;
                return true;
            }
        }
        if (size > 8) { return false; }
        if (size == 0) { return true; }

        if (addr >= 0x0c000000 && addr < 0x0c400000) {
            if (size != 4) { return false; }
            uint32_t v = 0;
            if (!plic.Read(addr - 0x0c000000, v)) return false;
            out = v;
            return true;
        }

        if (addr >= 0x10001000 && addr < 0x10002000) {
            if (size != 4) { out = 0; return true; }
            uint64_t offset = addr - 0x10001000;
            uint32_t out_val = 0;
            if (!virtio.Read(offset, out_val)) { return false; }
            out = out_val;
            return true;
        }

        if (addr >= 0x02000000 && addr < 0x02010000) {
            uint64_t offset = addr - 0x2000000;
            switch(offset) {
            case 0x4000: {
                out = mtimecmp;
                return true;
            }

            case 0xBFF8: {
                out = mtime;
                return true;
            }

            default:
                out = 0;
                return true;
            }
        }

        uint64_t ramBase = ram.GetRamBase();
        size_t ramSize = ram.GetRamSize();
        if (!ram.Contains(addr) || (size > ramSize - (addr - ramBase))) {
            return false;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            value |= static_cast<uint64_t>(ram.At(addr + i)) << (8 * i);
        }
        out = value;
        return true;
    }

    bool Write(uint64_t addr, size_t size, uint64_t value) {
        if (addr >= 0x10000000 && addr < 0x10000008) {
            if (size != 1) return false;
            uint64_t offset = addr - 0x10000000;
            switch(offset) {
            case 0: {
                std::cout.put(value & 0xFF);
                std::cout.flush();
                std::lock_guard<std::mutex> lk(rxMtx);
                if (ier & 2) RaiseInterrupt(10);
                return true;
            }

            case 1: {
                std::lock_guard<std::mutex> lk(rxMtx);
                ier = value;
                if ((!rxBuf.empty() && (ier & 1)) || (ier & 2)) {
                    RaiseInterrupt(10);
                }
                return true;
            }

            default:
                return true;
            }
        }
        if (size == 0) { return true; }
        if (size > 8) { return false; }

        if (addr >= 0x0c000000 && addr < 0x0c400000) {
            if (size != 4) return false;
            return plic.Write(addr - 0x0c000000, static_cast<uint32_t>(value));
        }

        if (addr >= 0x10001000 && addr < 0x10002000) {
            uint64_t offset = addr - 0x10001000;
            uint32_t val = static_cast<uint32_t>(value);
            virtio.Write(offset, val);
            return true;
        }

        if (addr >= 0x02000000 && addr < 0x02010000) {
            uint64_t offset = addr - 0x2000000;
            switch(offset) {
            case 0x4000: {
                mtimecmp = value;
                return true;
            }

            case 0xBFF8: {
                mtime = value;
                return true;
            }

            default:
                return true;
            }
        }

        uint64_t ramBase = ram.GetRamBase();
        size_t ramSize = ram.GetRamSize();
        if (!ram.Contains(addr) || (size > ramSize - (addr - ramBase))) return false;
        for (size_t i = 0; i < size; ++i) {
            ram.At(addr + i) = (value >> (8 * i)) & 0xFF;
        }
        return true;
    }

    bool WriteBytes(uint64_t addr, const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return true;
        uint64_t ramBase = ram.GetRamBase();
        size_t ramSize = ram.GetRamSize();
        if (!ram.Contains(addr) || (bytes.size() > ramSize - (addr - ramBase))) return false;
        for (size_t i = 0; i < bytes.size(); ++i) {
            ram.At(addr + i) = bytes[i];
        }
        return true;
    }

    uint64_t GetRamBase() {
        return ram.GetRamBase();
    }

    size_t GetRamSize() {
        return ram.GetRamSize();
    }
};

bool VirtIODevice::HandleNotify() {
    if (queue_ready != 1 || queue_num == 0) { return false; }
    uint64_t avail_idx_raw = 0;
    if (!bus->Read(avail_addr + 2, 2, avail_idx_raw)) { return false; }
    uint16_t avail_idx = avail_idx_raw & 0xFFFF;
    
    if (avail_idx == (last_avail_idx & 0xFFFF)) {
        return true;
    }

    uint64_t desc_idx = 0;
    if (!bus->Read(avail_addr + 4 + 2 * (last_avail_idx % queue_num), 2, desc_idx)) { return false; }

    uint64_t desc0_addr = desc_addr + desc_idx * 16;
    uint64_t header_addr = 0;
    if (!bus->Read(desc0_addr, 8, header_addr)) { return false; }

    uint64_t header_len = 0;
    if (!bus->Read(desc0_addr + 8, 4, header_len)) { return false; }

    uint64_t desc0_flags = 0;
    if (!bus->Read(desc0_addr + 12, 2, desc0_flags)) { return false; }

    uint64_t desc0_next = 0;
    if (!bus->Read(desc0_addr + 14, 2, desc0_next)) { return false; }

    uint64_t req_type = 0;
    if (!bus->Read(header_addr, 4, req_type)) { return false; }

    uint64_t sector = 0;
    if (!bus->Read(header_addr + 8, 8, sector)) { return false; }

    if ((desc0_flags & 0x1) == 0) { return false; }

    uint64_t desc1_addr = desc_addr + desc0_next * 16;
    uint64_t desc1_buf_addr = 0;
    if (!bus->Read(desc1_addr, 8, desc1_buf_addr)) { return false; }

    uint64_t desc1_buf_len = 0;
    if (!bus->Read(desc1_addr + 8, 4, desc1_buf_len)) { return false; }

    uint64_t desc1_flags = 0;
    if (!bus->Read(desc1_addr + 12, 2, desc1_flags)) { return false; }
    
    uint64_t desc1_next = 0;
    if (!bus->Read(desc1_addr + 14, 2, desc1_next)) { return false; }

    if (req_type == 0) {
        disk.seekg(sector * 512);
        std::vector<uint8_t> buf(desc1_buf_len);
        disk.read(reinterpret_cast<char*>(buf.data()), desc1_buf_len);
        if (!disk.good()) { return false; }
        if (!bus->WriteBytes(desc1_buf_addr, buf)) { return false; }
    }
    else if (req_type == 1) {
        std::vector<uint8_t> buf(desc1_buf_len);
        for (uint64_t i = 0; i < desc1_buf_len; ++i) {
            uint64_t byte = 0;
            if (!bus->Read(desc1_buf_addr + i, 1, byte)) return false;
            buf[i] = static_cast<uint8_t>(byte);
        }
        disk.seekp(sector * 512);
        disk.write(reinterpret_cast<const char*>(buf.data()), desc1_buf_len);
        if (!disk.good()) { return false; }
        disk.flush();   
    }
    else {
        return false;
    }

    uint64_t desc2_addr = desc_addr + desc1_next * 16;
    uint64_t desc2_status_byte_addr = 0;
    if (!bus->Read(desc2_addr, 8, desc2_status_byte_addr)) { return false; }

    uint64_t desc2_status_byte_len = 0;
    if (!bus->Read(desc2_addr + 8, 4, desc2_status_byte_len)) { return false; }

    if (!bus->Write(desc2_status_byte_addr, 1, 0)) { return false; }

    uint64_t used_idx = 0;
    if (!bus->Read(used_addr + 2, 2, used_idx)) { return false; }

    uint64_t used_ring_addr = used_addr + 4 + 8 * (used_idx % queue_num);

    if (!bus->Write(used_ring_addr, 4, static_cast<uint32_t>(desc_idx))) { return false; }
    if (!bus->Write(used_ring_addr + 4, 4, static_cast<uint32_t>(desc1_buf_len))) { return false; }

    if (!bus->Write(used_addr + 2, 2, static_cast<uint16_t>(used_idx + 1))) { return false; }

    last_avail_idx++;

    interrupt_status |= 1;
    bus->RaiseInterrupt(1);

    return true;
}

class ElfFile {
public:
    ElfFile(std::string fileName) : fileName(std::move(fileName)) {}

    struct Segment {
        uint32_t p_type   = 0;
        uint32_t p_flags  = 0;
        uint64_t p_offset = 0;
        uint64_t p_vaddr  = 0;
        uint64_t p_paddr  = 0;
        uint64_t p_filesz = 0;
        uint64_t p_memsz  = 0;
        uint64_t p_align  = 0;
    };

    struct ElfHeader {
        bool     is64        = false;
        uint64_t e_entry     = 0;
        uint64_t e_phoff     = 0;
        uint16_t e_phentsize = 0;
        uint16_t e_phnum     = 0;
        uint16_t e_type      = 0;
        uint16_t e_ehsize    = 0;
        uint16_t e_machine   = 0;
        uint32_t e_version   = 0;
    };

    bool LoadElfBinary() {
        std::ifstream file(fileName, std::ios::binary | std::ios::ate);
        if (!file.is_open()) { return false; }

        std::streamsize size = file.tellg();
        if (size <= 0) {
            return false;
        }
        file.seekg(0, std::ios::beg);

        rawData.resize(size);
        file.read(reinterpret_cast<char*>(rawData.data()), size);
        return file.good();
    }

    bool ReadNumber(size_t size, uint64_t offset, uint64_t& outValue) {
        if (size == 0 || size > 8) { return false; }
        if (offset > rawData.size() || size > rawData.size() - offset) { return false; }

        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            value |= (static_cast<uint64_t>(rawData[i + offset]) & 0xFF) << (i * 8);
        }
        outValue = value;
        return true;
    }

    bool ReadByte(uint64_t offset, uint8_t& outValue) {
        size_t size = 1;
        if (offset > rawData.size() || size > rawData.size() - offset) { return false; }

        uint8_t value = rawData[offset];
        outValue = value;
        return true;
    }

    bool ReadBytes(size_t count, uint64_t offset, std::vector<uint8_t>& outArray) {
        for (size_t i = 0; i < count; ++i) {
            uint8_t byte = 0;
            if (!ReadByte(offset + i, byte)) { return false; }
            outArray.push_back(byte);
        }
        return true;
    }

    bool ParseElf() {
        segments.clear();
        uint64_t magicByte = 0;
        if (!ReadNumber(4, 0, magicByte)) { return false; }
        if (magicByte != 0x464C457FU) {
            return false;
        }

        uint64_t EI_Version = 0;
        if (!ReadNumber(1, 0x06, EI_Version)) { return false; }
        if (EI_Version != 1) { return false; }

        uint64_t data = 0;
        if (!ReadNumber(1, 5, data) || data != 1) { return false; }

        uint64_t mode = 0;
        if(!ReadNumber(1, 4 , mode)) { return false; }
        if (mode == 1) { 
            (header.is64 = false);
        }
        else if (mode == 2){
            (header.is64 = true);
        }
        else {
            return false;
        }

        uint64_t phentsize = 0;
        uint64_t phnum     = 0;
        uint64_t e_type    = 0;
        uint64_t e_ehsize  = 0;
        uint64_t e_machine = 0;
        uint64_t e_version = 0;

        if (!ReadNumber(2, 0x12, e_machine)) { return false; }
        if (!ReadNumber(2, 0x10, e_type)) { return false; }
        if (!ReadNumber(4, 0x14, e_version)) { return false; }

        if (e_machine != 243) { return false; }
        if (e_type != 2)    { return false; }
        if (e_version != 1)    { return false; }
        
        if (header.is64) {
            if (!ReadNumber(8, 0x18, header.e_entry)) { return false; }
            if (!ReadNumber(8, 0x20, header.e_phoff)) { return false; }
            if (!ReadNumber(2, 0x36, phentsize)) { return false; }
            if (!ReadNumber(2, 0x38, phnum)) { return false; }
            if (!ReadNumber(2, 0x34, e_ehsize)) { return false; }
        }
        else {
            if (!ReadNumber(4, 0x18, header.e_entry)) { return false; }
            if (!ReadNumber(4, 0x1C, header.e_phoff)) { return false; }
            if (!ReadNumber(2, 0x2A, phentsize)) { return false; }
            if (!ReadNumber(2, 0x2C, phnum)) { return false; }            
            if (!ReadNumber(2, 0x28, e_ehsize)) { return false; }
        }
        header.e_phentsize = static_cast<uint16_t>(phentsize);
        header.e_phnum = static_cast<uint16_t>(phnum);
        header.e_type = static_cast<uint16_t>(e_type);
        header.e_ehsize = static_cast<uint16_t>(e_ehsize);
        header.e_machine = static_cast<uint16_t>(e_machine);
        header.e_version = static_cast<uint32_t>(e_version);
        
        const uint64_t ehSize = header.is64 ? 64 : 52;
        const uint64_t phSize = header.is64 ? 56 : 32;
        if (header.e_ehsize != ehSize)     { return false; }
        if (header.e_phentsize != phSize)  { return false; }
        if (rawData.size() < ehSize) { return false; }

        const uint64_t tableSize = static_cast<uint64_t>(header.e_phnum) * header.e_phentsize;
        if (header.e_phoff > rawData.size() || tableSize > rawData.size() - header.e_phoff) { return false; }

        for (uint64_t i = 0; i < header.e_phnum; ++i) {
            uint64_t off = header.e_phoff + i * header.e_phentsize;

            Segment seg;
            uint64_t type = 0, flags = 0;

            if (header.is64) {
                if (!ReadNumber(4, off + 0,  type))         { return false; }
                if (!ReadNumber(4, off + 4,  flags))        { return false; }
                if (!ReadNumber(8, off + 8,  seg.p_offset)) { return false; }
                if (!ReadNumber(8, off + 16, seg.p_vaddr))  { return false; }
                if (!ReadNumber(8, off + 24, seg.p_paddr))  { return false; }
                if (!ReadNumber(8, off + 32, seg.p_filesz)) { return false; }
                if (!ReadNumber(8, off + 40, seg.p_memsz))  { return false; }
                if (!ReadNumber(8, off + 48, seg.p_align))  { return false; }
            } else {
                if (!ReadNumber(4, off + 0,  type))         { return false; }
                if (!ReadNumber(4, off + 4,  seg.p_offset)) { return false; }
                if (!ReadNumber(4, off + 8,  seg.p_vaddr))  { return false; }
                if (!ReadNumber(4, off + 12, seg.p_paddr))  { return false; }
                if (!ReadNumber(4, off + 16, seg.p_filesz)) { return false; }
                if (!ReadNumber(4, off + 20, seg.p_memsz))  { return false; }
                if (!ReadNumber(4, off + 24, flags))        { return false; }
                if (!ReadNumber(4, off + 28, seg.p_align))  { return false; }
            }
            seg.p_type  = static_cast<uint32_t>(type);
            seg.p_flags = static_cast<uint32_t>(flags);
            
            if (seg.p_type == 1) {   // PT_LOAD
                if (seg.p_filesz > seg.p_memsz) { return false; }
                if (seg.p_offset > rawData.size() || seg.p_filesz > rawData.size() - seg.p_offset) { return false; }
                if (seg.p_align > 1) {
                    if ((seg.p_vaddr % seg.p_align) != (seg.p_offset % seg.p_align)) { return false; }
                    if ((seg.p_align & (seg.p_align - 1)) != 0) { return false; }
                }
            }
            segments.push_back(seg);
        }
        return true;
    }

    const ElfHeader& Header() const {
        return header;
    }

    uint64_t Entry() const {
        return header.e_entry;
    }

    const std::vector<Segment>& Segments() const {
        return segments;
    }
private:
    std::string fileName;
    std::vector<uint8_t> rawData;

    ElfHeader header;
    std::vector<Segment> segments;
};

class Loader {
private:
    Bus& bus;
    ElfFile& parsedElf;

public:
    Loader(Bus& bus, ElfFile& elf) : bus(bus), parsedElf(elf) {};

    bool Load() {
        bool wasPT_LOADSeg = false;

        for (const auto& seg : parsedElf.Segments()) {
            if (seg.p_type != 1) {
                continue;
            }
            if (seg.p_paddr < bus.GetRamBase()) { return false; }
            uint64_t offset = seg.p_paddr - bus.GetRamBase();
            if (offset > bus.GetRamSize()) { return false; }
            if (seg.p_memsz > bus.GetRamSize() - offset) { return false; }
        }

        for (const auto& seg : parsedElf.Segments()) {
            if (seg.p_type != 1) {
                continue;
            }
            wasPT_LOADSeg = true;
            std::vector<uint8_t> bytesToWrite;
            if (!parsedElf.ReadBytes(seg.p_filesz, seg.p_offset, bytesToWrite)) { return false; }
            if (!bus.WriteBytes(seg.p_paddr, bytesToWrite)) { return false; }
            if (seg.p_memsz > seg.p_filesz) {
                uint64_t length = seg.p_memsz - seg.p_filesz;
                uint64_t zeroOffset = seg.p_paddr + seg.p_filesz;
                std::vector<uint8_t> zeroBytes(length, 0);
                if (!bus.WriteBytes(zeroOffset, zeroBytes)) { return false; }
            }
        }
        return wasPT_LOADSeg;
    }
};

class RV64I {
private:
    Bus& bus;
    uint64_t pc = 0;
    std::array<uint64_t, 32> x{};
    uint64_t mstatus = 0;
    uint64_t mtvec = 0;
    uint64_t mepc = 0;
    uint64_t mcause = 0;
    uint64_t mtval = 0;
    uint64_t mscratch = 0;
    uint64_t mie = 0;
    uint8_t mode = 3;
    uint64_t medeleg = 0;
    uint64_t mideleg = 0;
    uint64_t stvec = 0;
    uint64_t sepc = 0;
    uint64_t scause = 0;
    uint64_t stval = 0;
    uint64_t sscratch = 0;
    uint64_t satp = 0;
    uint64_t reservation_addr = 0;
    uint64_t stimecmp = 0;
    uint64_t menvcfg = 0;
    
    bool reservation_valid = false;

public:
    RV64I(Bus& bus) : bus(bus) {};
    const uint64_t SSTATUS_MASK = 0x80000003000DE133ULL;

    void ResetCPU(uint64_t e_entry) {
        std::fill(x.begin(), x.end(), 0);
        mode = 3;
        pc = e_entry;
        reservation_valid = false;
    }

    uint64_t GetPC() {
        return pc;
    }

    bool TranslateAddress(uint64_t& va, uint64_t& pa, AccessType accessType) {
        auto RaiseTrapBasedOnType = [this](AccessType accessType, uint64_t& va) -> bool {
            switch (accessType) {
                case AccessType::Load:
                    RaiseTrap(TrapCause::LoadPageFault, va);
                    break;

                case AccessType::Store:
                    RaiseTrap(TrapCause::StorePageFault, va);
                    break;

                case AccessType::Fetch:
                    RaiseTrap(TrapCause::InstructionPageFault, va);
                    break;
            }
            return false; 
        };

        if (mode == 3 || satp >> 60 == 0) {
            pa = va;
            return true;
        }

        if (satp >> 60 == 8) {
            if (((int64_t)(va << 25) >> 25) != va) {
                return RaiseTrapBasedOnType(accessType, va);
            }
            uint64_t table_addr = (satp & 0xFFFFFFFFFFF) << 12;
            uint64_t vpn2 = (va >> 30) & 0b111111111;
            uint64_t vpn1 = (va >> 21) & 0b111111111;
            uint64_t vpn0 = (va >> 12) & 0b111111111;
            uint32_t offset = va & 0b111111111111;

            for (int i = 2; i >= 0; --i) {
                uint64_t vpn = 0;
                if (i == 2) { vpn = vpn2; }
                if (i == 1) { vpn = vpn1; }
                if (i == 0) { vpn = vpn0; }

                uint64_t pte_addr = table_addr + vpn * 8;
                uint64_t pte = 0;
                if (!bus.Read(pte_addr, 8, pte)) { return RaiseTrapBasedOnType(accessType, va); }
                if ((pte & 0b1) == 0) { return RaiseTrapBasedOnType(accessType, va); }

                if ((pte & 0b1110) == 0) {
                    table_addr = ((pte >> 10) & 0xFFFFFFFFFFF) << 12;
                }
                
                else {
                    if ((pte & 0b10000) == 0 && mode == 0) { 
                        return RaiseTrapBasedOnType(accessType, va);
                    }
                    if (accessType == AccessType::Load && (pte & 0b10) == 0) { return RaiseTrapBasedOnType(accessType, va); }
                    if (accessType == AccessType::Store && ((pte & 0b100) == 0 || (pte & 0b10) == 0)) { return RaiseTrapBasedOnType(accessType, va); }
                    if (accessType == AccessType::Fetch && (pte & 0b1000) == 0) { return RaiseTrapBasedOnType(accessType, va); }

                    uint64_t pnn = (pte >> 10) & 0xFFFFFFFFFFF;

                    if (i == 2 && (pnn & 0x3FFFF) != 0) { return RaiseTrapBasedOnType(accessType, va); }
                    if (i == 1 && (pnn & 0x1FF) != 0) { return RaiseTrapBasedOnType(accessType, va); }
                    
                    if (i == 2) { offset = vpn1 << 21 | vpn0 << 12 | offset; }
                    if (i == 1) { offset = vpn0 << 12 | offset; }

                    pa = (pnn << 12) | offset;
                    return true;
                }
            }

            return RaiseTrapBasedOnType(accessType, va);
        }

        else {
            return false;
        }
    }

    bool ReadCSR(uint16_t addr, uint64_t& out) {
        uint8_t required_mode = (addr >> 8) & 0b11;
        if (mode < required_mode) {
            return false;
        }

        switch(addr) {
        case 0x100:
            out = mstatus & SSTATUS_MASK; 
            return true;

        case 0x104:
            out = mie & mideleg;
            return true;

        case 0x105:
            out = stvec;
            return true;
        
        case 0x140:
            out = sscratch;
            return true;

        case 0x141:
            out = sepc;
            return true;

        case 0x142:
            out = scause;
            return true;
        
        case 0x143:
            out = stval;
            return true;

        case 0x144: {
            uint64_t stip = (stimecmp != 0 && bus.GetMtime() >= stimecmp) ? (1ULL << 5) : 0;
            out = (stip | (bus.IsExternalInterruptPending() ? (1ULL << 9) : 0)) & mideleg;
            return true;
        }

        case 0x14D:
            out = stimecmp;
            return true;

        case 0x180:
            out = satp;
            return true;

        case 0x300:
            out = mstatus;
            return true;

        case 0x302:
            out = medeleg;
            return true;

        case 0x303:
            out = mideleg;
            return true;

        case 0x304:
            out = mie;
            return true;
        
        case 0x305:
            out = mtvec;
            return true;

        case 0x306:
            out = 0;
            return true;

        case 0x30A:
            out = menvcfg;
            return true;

        case 0x340:
            out = mscratch;
            return true;

        case 0x341:
            out = mepc;
            return true;

        case 0x342:
            out = mcause;
            return true;

        case 0x343:
            out = mtval;
            return true;

        case 0x344:
            out = (bus.IsTimerInterruptPending() ? (1ULL << 7) : 0) | (bus.IsExternalInterruptPending() ? (1ULL << 9) : 0);
            return true;

        case 0x3A0 ... 0x3AF:
            out = 0;
            return true;

        case 0x3B0 ... 0x3EF:
            out = 0;
            return true;

        case 0xC01:
            out = bus.GetMtime();
            return true;

        case 0xF14:
            out = 0;
            return true;

        default:
            return false;
        }
    }
  
    bool WriteCSR(uint16_t addr, uint64_t val) {
        if (mode < ((addr >> 8) & 0b11)) return false;
        if (((addr >> 10) & 0b11) == 0b11) return false;

        switch(addr) {
        case 0x100:
            mstatus = (mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK); 
            return true;

        case 0x104:
            mie = (mie & ~mideleg) | (val & mideleg);
            return true;

        case 0x105:
            stvec = val;
            return true;
        
        case 0x140:
            sscratch = val;
            return true;

        case 0x141:
            sepc = val & ~1ULL;
            return true;

        case 0x142:
            scause = val;
            return true;
        
        case 0x143:
            stval = val;
            return true;

        case 0x144:
            return true;

        case 0x14D:
            stimecmp = val;
            return true;

        case 0x180:
            satp = val;
            return true;
        
        case 0x300:
            mstatus = val;
            return true;

        case 0x302:
            medeleg = val;
            return true;

        case 0x303:
            mideleg = val;
            return true;

        case 0x304:
            mie = val;
            return true;
        
        case 0x305:
            mtvec = val;
            return true;

        case 0x306:
            return true;

        case 0x30A:
            menvcfg = val;
            return true;

        case 0x340:
            mscratch = val;
            return true;

        case 0x341:
            mepc = val & ~1ULL;
            return true;

        case 0x342:
            mcause = val;
            return true;

        case 0x343:
            mtval = val;
            return true;

        case 0x344:
            return true;

        case 0x3A0 ... 0x3AF:
        case 0x3B0 ... 0x3EF:
            return true;

        default:
            return false;
        }
    }

    bool RaiseTrap(TrapCause cause, uint64_t val = 0) {
        bool is_delegated = (mode <= 1) && ((medeleg & (1ULL << (uint64_t)cause)) != 0);
        if (is_delegated) {
            sepc = pc;
            scause = (uint64_t)cause;
            stval = val;
            uint64_t sie = (mstatus >> 1) & 1;
            mstatus &= ~((1ULL << 1) | (1ULL << 5));
            mstatus |= (sie << 5);
            if (mode == 1) mstatus |= (1ULL << 8);
            else mstatus &= ~(1ULL << 8);
            mode = 1;
            if (stvec == 0) {
                return false;
            }
            else {
                pc = stvec;
                return true;
            }
        }
        else {
            mstatus = (mstatus & ~(3ULL << 11)) | ((uint64_t)mode << 11);
            uint64_t mie_bit = (mstatus >> 3) & 1;
            mstatus &= ~((1ULL << 3) | (1ULL << 7));
            mstatus |= (mie_bit << 7);
            mode = 3;
            mepc = pc;
            mcause = (uint64_t)cause;
            mtval = val;
            if (mtvec == 0) {
                return false;
            }
            else {
                pc = mtvec;
                return true;
            }
        }
    }

    bool Step() {
        if ((pc & 0b11) != 0) { 
            return RaiseTrap(TrapCause::InstructionAddressMisaligned); 
        }
        
        bus.Tick();

        bool s_irq_enabled = (mode == 0) || ((mode == 1) && ((mstatus & (1ULL << 1)) != 0));

        if (s_irq_enabled && (mideleg & (1ULL << 9)) && (mie & (1ULL << 9)) && bus.IsExternalInterruptPending()) {
            sepc = pc;
            scause = (1ULL << 63) | 9;
            uint64_t sie = (mstatus >> 1) & 1;
            mstatus &= ~((1ULL << 1) | (1ULL << 5));
            mstatus |= (sie << 5);
            if (mode == 1) mstatus |= (1ULL << 8);
            else mstatus &= ~(1ULL << 8);
            mode = 1;
            pc = stvec;
            return true;
        }

        if (s_irq_enabled && stimecmp != 0 && bus.GetMtime() >= stimecmp && (mideleg & (1ULL << 5)) && (mie & (1ULL << 5))) {
            sepc = pc;
            scause = (1ULL << 63) | 5;
            uint64_t sie = (mstatus >> 1) & 1;
            mstatus &= ~((1ULL << 1) | (1ULL << 5));
            mstatus |= (sie << 5);
            if (mode == 1) mstatus |= (1ULL << 8);
            else mstatus &= ~(1ULL << 8);
            mode = 1;
            pc = stvec;
            return true;
        }

        bool m_irq_enabled = (mode < 3) || ((mstatus & (1ULL << 3)) != 0);
        if (m_irq_enabled && bus.IsTimerInterruptPending() && ((mie & (1ULL << 7)) != 0)) {
            mstatus = (mstatus & ~(3ULL << 11)) | ((uint64_t)mode << 11);
            uint64_t mie_bit = (mstatus >> 3) & 1;
            mstatus &= ~((1ULL << 3) | (1ULL << 7));
            mstatus |= (mie_bit << 7);
            mode = 3;
            mepc = pc;
            mcause = (1ULL << 63) | 7;
            pc = mtvec;
            return true;
        }

        uint64_t code = 0;
        uint64_t phys_pc = 0;
        if (!TranslateAddress(pc, phys_pc, AccessType::Fetch)) { return true; }
        if (!bus.Read(phys_pc, 4, code)) { return RaiseTrap(TrapCause::InstructionAccessFault); }

        uint8_t opcode = code & 0b01111111; 
        uint8_t registerDest = (code >> 7) & 0b00011111;
        uint8_t funct3 = (code >> 12) & 0b00000111;
        uint8_t registerSource1 = (code >> 15) & 0b00011111;
        uint8_t registerSource2 = (code >> 20) & 0b00011111;
        uint8_t funct7 = (code >> 25) & 0b01111111;
        int32_t numb = static_cast<int32_t>(code) >> 20;

        switch(opcode) {
        // SYSTEM (ECALL, EBREAK)
        case 0x73: {
            switch(funct3) {
            case 0b000:
                if (funct7 == 0b0001001) {
                    if (mode < 1) return RaiseTrap(TrapCause::IllegalInstruction);
                    pc += 4; 
                    return true;
                }
                
                switch(code >> 8) {
                // ECALL
                case 0x000000: {
                    if (mode == 0) return RaiseTrap(TrapCause::UserEnvCall);
                    if (mode == 1) return RaiseTrap(TrapCause::SupervisorEnvCall);
                    return RaiseTrap(TrapCause::MachineEnvCall);
                }

                // MRET
                case 0x302000: {
                    if (mode < 3) return RaiseTrap(TrapCause::IllegalInstruction);
                    uint64_t mpp  = (mstatus >> 11) & 3;
                    uint64_t mpie = (mstatus >> 7) & 1;
                    mstatus &= ~(1ULL << 3);       // MIE = 0
                    mstatus |= (mpie << 3);        // MIE = MPIE
                    mstatus |= (1ULL << 7);        // MPIE = 1
                    mstatus &= ~(3ULL << 11);      // MPP = U
                    mode = mpp;
                    pc = mepc;
                    return true;
                }

                // EBREAK
                case 0x001000: {
                    return RaiseTrap(TrapCause::Breakpoint);
                }

                // SRET
                case 0x102000: {
                    if (mode < 1) return RaiseTrap(TrapCause::IllegalInstruction);
                    uint64_t spp  = (mstatus >> 8) & 1;
                    uint64_t spie = (mstatus >> 5) & 1;
                    mstatus &= ~(1ULL << 1);   // SIE = 0
                    mstatus |= (spie << 1);    // SIE = SPIE
                    mstatus |= (1ULL << 5);    // SPIE = 1
                    mstatus &= ~(1ULL << 8);   // SPP = 0
                    mode = spp;
                    pc = sepc;
                    return true;
                }

                // WFI
                case 0x105000: {
                    pc += 4;
                    return true;
                }

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
            
            case 0b001: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (!WriteCSR(csr, x[registerSource1])) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            case 0b010: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerSource1 != 0) {
                    if (!WriteCSR(csr, old_val | x[registerSource1])) {
                        return RaiseTrap(TrapCause::IllegalInstruction);
                    }
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            case 0b011: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerSource1 != 0) {
                    if (!WriteCSR(csr, old_val & ~x[registerSource1])) {
                        return RaiseTrap(TrapCause::IllegalInstruction);
                    }
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            case 0b101: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (!WriteCSR(csr, registerSource1)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            case 0b110: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerSource1 != 0) {
                    if (!WriteCSR(csr, old_val | registerSource1)) {
                        return RaiseTrap(TrapCause::IllegalInstruction);
                    }
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            case 0b111: {
                uint16_t csr = code >> 20;
                uint64_t old_val = 0;
                if (!ReadCSR(csr, old_val)) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                if (registerSource1 != 0) {
                    if (!WriteCSR(csr, old_val & ~registerSource1)) {
                        return RaiseTrap(TrapCause::IllegalInstruction);
                    }
                }

                if (registerDest != 0) {
                    x[registerDest] = old_val; 
                }
                pc += 4;
                return true;
            }

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }

        // FENCE (Memory ordering barrier)
        case 0x0F: {
            pc += 4;
            return true;
        }

        // OP-IMM (I-type ALU instructions)
        case 0x13:
            switch(funct3) {
            // ADDI
            case 0b000:
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] + numb;
                }
                pc += 4;
                return true;

            // SLLI
            case 0b001: {
                uint32_t buf = code >> 26;
                if (buf != 0b000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    uint32_t N = (code >> 20) & 0b111111;
                    x[registerDest] = x[registerSource1] << N;
                }
                pc += 4;
                return true;
            }

            // SLTI
            case 0b010:
                if (registerDest != 0) {
                    x[registerDest] = (static_cast<int64_t>(x[registerSource1]) < numb) ? 1 : 0;
                }
                pc += 4;
                return true;

            // SLTIU
            case 0b011: {
                uint64_t I = static_cast<uint64_t>(numb);
                if (registerDest != 0) {
                    x[registerDest] = (x[registerSource1] < I) ? 1 : 0;
                }
                pc += 4;
                return true;
            }

            // XORI
            case 0b100:
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] ^ numb;
                }
                pc += 4;
                return true;

            // SRLI / SRAI
            case 0b101: {
                uint32_t buf = code >> 26;
                uint32_t N = (code >> 20) & 0b111111;
                switch (buf) {
                // SRLI
                case 0b000000:
                    if (registerDest != 0) {
                        x[registerDest] = x[registerSource1] >> N;
                    }
                    pc += 4;
                    return true;

                // SRAI
                case 0b010000:
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(x[registerSource1]) >> N;
                    }
                    pc += 4;
                    return true;

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);;
                }
            }

            // ORI
            case 0b110:
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] | numb;
                }
                pc += 4;
                return true;

            // ANDI
            case 0b111:
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] & numb;
                }
                pc += 4;
                return true;

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }

        // OP (R-type ALU instructions)
        case 0x33: {
            uint8_t N = x[registerSource2] & 0b111111;
            if (funct7 == 0x01) {
                switch(funct3) {
                case 0b000: {
                    if (registerDest != 0) {
                        x[registerDest] =  x[registerSource1] * x[registerSource2];
                    }
                    pc += 4;
                    return true;
                }

                case 0b001: {
                    if (registerDest != 0) {
                        x[registerDest] = ((__int128_t)(int64_t)x[registerSource1] * (__int128_t)(int64_t)x[registerSource2]) >> 64;
                    }
                    pc += 4;
                    return true;
                }

                case 0b010: {
                    if (registerDest != 0) {
                        x[registerDest] = ((__int128_t)(int64_t)x[registerSource1] * (unsigned __int128)(uint64_t)x[registerSource2]) >> 64;
                    }
                    pc += 4;
                    return true;
                }

                case 0b011: {
                    if (registerDest != 0) {
                        x[registerDest] = ((unsigned __int128)(uint64_t)x[registerSource1] * (unsigned __int128)(uint64_t)x[registerSource2]) >> 64;
                    }
                    pc += 4;
                    return true;
                }

                case 0b100: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = -1ULL;
                        }
                        else if (x[registerSource1] == INT64_MIN && x[registerSource2] == -1) {
                            x[registerDest] = INT64_MIN;
                        }
                        else {
                            x[registerDest] = (int64_t)x[registerSource1] / (int64_t)x[registerSource2];
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b101: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = UINT64_MAX;
                        }
                        else {
                            x[registerDest] = (uint64_t)x[registerSource1] / (uint64_t)x[registerSource2];
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b110: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = x[registerSource1];
                        }
                        else if (x[registerSource1] == INT64_MIN && x[registerSource2] == -1) {
                            x[registerDest] = 0;
                        }
                        else {
                            x[registerDest] = (int64_t)x[registerSource1] % (int64_t)x[registerSource2];
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b111: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = x[registerSource1];
                        }
                        else {
                            x[registerDest] = (uint64_t)x[registerSource1] % (uint64_t)x[registerSource2];
                        }
                    }
                    pc += 4;
                    return true;
                }

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
            }

            switch (funct3) {
            // ADD / SUB
            case 0b000:
                switch (funct7) {
                // ADD
                case 0b0000000:
                    if (registerDest != 0) {
                        x[registerDest] = x[registerSource1] + x[registerSource2];
                    }
                    pc += 4;
                    return true;

                // SUB
                case 0b0100000:
                    if (registerDest != 0) {
                        x[registerDest] = x[registerSource1] - x[registerSource2];
                    }
                    pc += 4;
                    return true;

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);;
                }

            // SLL
            case 0b001:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] << N;
                }
                pc += 4;
                return true;

            // SLT
            case 0b010:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = (static_cast<int64_t>(x[registerSource1]) < static_cast<int64_t>(x[registerSource2])) ? 1 : 0;
                }
                pc += 4;
                return true;

            // SLTU
            case 0b011:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = (x[registerSource1] < x[registerSource2]) ? 1 : 0;
                }
                pc += 4;
                return true;

            // XOR
            case 0b100:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] ^ x[registerSource2];
                }
                pc += 4;
                return true;

            // SRL / SRA
            case 0b101:
                switch (funct7) {
                // SRL
                case 0b0000000:
                    if (registerDest != 0) {
                        x[registerDest] = x[registerSource1] >> N;
                    }
                    pc += 4;
                    return true;

                // SRA
                case 0b0100000:
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(x[registerSource1]) >> N;
                    }
                    pc += 4;
                    return true;

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

            // OR
            case 0b110:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] | x[registerSource2];
                }
                pc += 4;
                return true;

            // AND
            case 0b111:
                if (funct7 != 0b0000000) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = x[registerSource1] & x[registerSource2];
                }
                pc += 4;
                return true;

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }

        // AUIPC (Add Upper Immediate to PC)
        case 0x17: {
            int32_t signed_offset = static_cast<int32_t>(code & 0xFFFFF000);
            if (registerDest != 0) {
                x[registerDest] = signed_offset + pc;
            }
            pc += 4;
            return true;
        }
        
        // LOAD (I-type load instructions)
        case 0x03: {
            uint64_t va_addr = x[registerSource1] + numb;
            uint64_t addr = 0;
            if (!TranslateAddress(va_addr, addr, AccessType::Load)) { return true; }

            switch (funct3) {
            // LW (Load Word, sign-extended)
            case 0b010: {
                uint64_t data = 0;
                if (!bus.Read(addr, 4, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    int64_t signed_data = static_cast<int64_t>(static_cast<int32_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }

            // LB (Load Byte, sign-extended)
            case 0b000: {
                uint64_t data = 0;
                if (!bus.Read(addr, 1, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    int64_t signed_data = static_cast<int64_t>(static_cast<int8_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }

            // LBU (Load Byte Unsigned, zero-extended)
            case 0b100: {
                uint64_t data = 0;
                if (!bus.Read(addr, 1, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    uint64_t signed_data = static_cast<uint64_t>(static_cast<uint8_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }
                
            // LH (Load Halfword, sign-extended)
            case 0b001: {
                uint64_t data = 0;
                if (!bus.Read(addr, 2, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    int64_t signed_data = static_cast<int64_t>(static_cast<int16_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }

            // LHU (Load Halfword Unsigned, zero-extended)
            case 0b101: {
                uint64_t data = 0;
                if (!bus.Read(addr, 2, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    uint64_t signed_data = static_cast<uint64_t>(static_cast<uint16_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }

            // LD (Load Doubleword, 64-bit)
            case 0b011: {
                uint64_t data = 0;
                if (!bus.Read(addr, 8, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    x[registerDest] = data;
                }
                pc += 4;
                return true;
            }

            // LWU (Load Word Unsigned, zero-extended)
            case 0b110: {
                uint64_t data = 0;
                if (!bus.Read(addr, 4, data)) { return RaiseTrap(TrapCause::LoadAccessFault); }
                if (registerDest != 0) {
                    uint64_t signed_data = static_cast<uint64_t>(static_cast<uint32_t>(data));
                    x[registerDest] = signed_data;
                }
                pc += 4;
                return true;
            }

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }
        
        // STORE (S-type store instructions)
        case 0x23: {
            int32_t offset = static_cast<int32_t>((code >> 25) << 5) | ((code >> 7) & 0b11111);
            if ((offset & (1 << 11)) != 0) {
                offset -= (1 << 12);
            }
            uint64_t va_addr = x[registerSource1] + offset;
            uint64_t addr = 0;
            if (!TranslateAddress(va_addr, addr, AccessType::Store)) { return true; }

            switch (funct3) {
            // SW (Store Word, 32-bit)
            case 0b010: {
                if (!bus.Write(addr, 4, x[registerSource2])) { return RaiseTrap(TrapCause::StoreAccessFault); }
                pc += 4;
                return true;
            }

            // SB (Store Byte, 8-bit)
            case 0b000: {
                if (!bus.Write(addr, 1, x[registerSource2])) { return RaiseTrap(TrapCause::StoreAccessFault); }
                pc += 4;
                return true;
            }

            // SH (Store Halfword, 16-bit)
            case 0b001: {
                if (!bus.Write(addr, 2, x[registerSource2])) { return RaiseTrap(TrapCause::StoreAccessFault); }
                pc += 4;
                return true;
            }

            // SD (Store Doubleword, 64-bit)
            case 0b011: {
                if (!bus.Write(addr, 8, x[registerSource2])) { return RaiseTrap(TrapCause::StoreAccessFault); }
                pc += 4;
                return true;
            }

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }

        // AMO (Atomic Memory Operations - RV64A)
        case 0x2F: {
            uint8_t funct5 = (code >> 27) & 0b11111;
            uint64_t va_addr = x[registerSource1];
            uint64_t addr = 0;
            size_t size = 0;

            if (funct3 == 0b010) {
                // 32-bit Word (.w)
                if ((va_addr & 0b11) != 0) {
                    return RaiseTrap(TrapCause::StoreAddressMisaligned, va_addr);
                }
                size = 4;
            } else if (funct3 == 0b011) {
                // 64-bit Doubleword (.d)
                if ((va_addr & 0b111) != 0) {
                    return RaiseTrap(TrapCause::StoreAddressMisaligned, va_addr);
                }
                size = 8;
            } else {
                return RaiseTrap(TrapCause::IllegalInstruction);
            }

            // LR (Load Reserved)
            if (funct5 == 0b00010) {
                if (registerSource2 != 0) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (!TranslateAddress(va_addr, addr, AccessType::Load)) { return true; }
                uint64_t data = 0;
                if (!bus.Read(addr, size, data)) { return RaiseTrap(TrapCause::LoadAccessFault, va_addr); }
                if (size == 4) {
                    data = static_cast<int64_t>(static_cast<int32_t>(data));
                }
                if (registerDest != 0) {
                    x[registerDest] = data;
                }
                reservation_addr = addr;
                reservation_valid = true;
                pc += 4;
                return true;
            }

            // SC (Store Conditional)
            if (funct5 == 0b00011) {
                if (!TranslateAddress(va_addr, addr, AccessType::Store)) { return true; }
                if (reservation_valid && reservation_addr == addr) {
                    if (!bus.Write(addr, size, x[registerSource2])) { return RaiseTrap(TrapCause::StoreAccessFault, va_addr); }
                    if (registerDest != 0) {
                        x[registerDest] = 0; // Success (0)
                    }
                } else {
                    if (registerDest != 0) {
                        x[registerDest] = 1; // Failure (non-zero)
                    }
                }
                reservation_valid = false;
                pc += 4;
                return true;
            }

            // Read-Modify-Write AMO instructions
            if (!TranslateAddress(va_addr, addr, AccessType::Store)) { return true; }
            uint64_t old_val = 0;
            if (!bus.Read(addr, size, old_val)) { return RaiseTrap(TrapCause::LoadAccessFault, va_addr); }

            uint64_t new_val = 0;
            if (size == 4) {
                int32_t old32 = static_cast<int32_t>(old_val);
                int32_t src2 = static_cast<int32_t>(x[registerSource2]);
                int32_t res32 = 0;

                switch (funct5) {
                // AMOSWAP.W
                case 0b00001: res32 = src2; break;
                // AMOADD.W
                case 0b00000: res32 = old32 + src2; break;
                // AMOXOR.W
                case 0b00100: res32 = old32 ^ src2; break;
                // AMOAND.W
                case 0b01100: res32 = old32 & src2; break;
                // AMOOR.W
                case 0b01000: res32 = old32 | src2; break;
                // AMOMIN.W (signed)
                case 0b10000: res32 = std::min(old32, src2); break;
                // AMOMAX.W (signed)
                case 0b10100: res32 = std::max(old32, src2); break;
                // AMOMINU.W (unsigned)
                case 0b11000: res32 = static_cast<int32_t>(std::min(static_cast<uint32_t>(old32), static_cast<uint32_t>(src2))); break;
                // AMOMAXU.W (unsigned)
                case 0b11100: res32 = static_cast<int32_t>(std::max(static_cast<uint32_t>(old32), static_cast<uint32_t>(src2))); break;
                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                new_val = static_cast<uint32_t>(res32);
                if (registerDest != 0) {
                    x[registerDest] = static_cast<int64_t>(old32);
                }
            } else {
                uint64_t src2 = x[registerSource2];
                uint64_t res64 = 0;

                switch (funct5) {
                // AMOSWAP.D
                case 0b00001: res64 = src2; break;
                // AMOADD.D
                case 0b00000: res64 = old_val + src2; break;
                // AMOXOR.D
                case 0b00100: res64 = old_val ^ src2; break;
                // AMOAND.D
                case 0b01100: res64 = old_val & src2; break;
                // AMOOR.D
                case 0b01000: res64 = old_val | src2; break;
                // AMOMIN.D (signed)
                case 0b10000: res64 = static_cast<uint64_t>(std::min(static_cast<int64_t>(old_val), static_cast<int64_t>(src2))); break;
                // AMOMAX.D (signed)
                case 0b10100: res64 = static_cast<uint64_t>(std::max(static_cast<int64_t>(old_val), static_cast<int64_t>(src2))); break;
                // AMOMINU.D (unsigned)
                case 0b11000: res64 = std::min(old_val, src2); break;
                // AMOMAXU.D (unsigned)
                case 0b11100: res64 = std::max(old_val, src2); break;
                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }

                new_val = res64;
                if (registerDest != 0) {
                    x[registerDest] = old_val;
                }
            }

            if (!bus.Write(addr, size, new_val)) { return RaiseTrap(TrapCause::StoreAccessFault, va_addr); }
            pc += 4;
            return true;
        }

        // JAL (Jump and Link)
        case 0x6F: {
            uint32_t offset = code >> 31;
            offset = (offset << 8) | ((code >> 12) & 0b11111111);
            offset = (offset << 1) | ((code >> 20) & 0b1);
            offset = (offset << 10) | ((code >> 21) & 0b1111111111);
            offset = offset << 1;
            int32_t signed_offset = static_cast<int32_t>(offset);
            if ((signed_offset & (1 << 20)) != 0) {
                signed_offset -= (1 << 21);
            }
            uint64_t target = pc + signed_offset;
            if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
            pc += 4;
            if (registerDest != 0) {
                x[registerDest] = pc;  
            }
            pc = target;
            return true;
        }

        // LUI (Load Upper Immediate)
        case 0x37: {
            int32_t signed_offset = static_cast<int32_t>(code & 0xFFFFF000);
            if (registerDest != 0) {
                x[registerDest] = static_cast<int64_t>(signed_offset);
            }
            pc += 4;
            return true;
        }

        // JALR (Jump and Link Register)
        case 0x67: {
            switch (funct3) {
            // JALR
            case 0b000: {
                uint64_t storedPc = pc;
                uint64_t target = (x[registerSource1] + numb) & 0xFFFFFFFFFFFFFFFE;
                if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                if (registerDest != 0) {
                    x[registerDest] = storedPc + 4;
                }
                pc = target;
                return true;
            }

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }

        // BRANCH (B-type conditional branches)
        case 0x63: {
            uint32_t offset = 0;
            offset = (offset << 1) | ((code >> 31) & 0b1);
            offset = (offset << 1) | ((code >> 7) & 0b1);
            offset = (offset << 6) | ((code >> 25) & 0b111111);
            offset = (offset << 4) | ((code >> 8) & 0b1111);
            offset <<= 1;
            int32_t signed_offset = static_cast<int32_t>(offset);
            if ((signed_offset & (1 << 12)) != 0) {
                signed_offset -= (1 << 13);
            }
            switch (funct3) {
            // BEQ (Branch if Equal)
            case 0b000: {
                if (x[registerSource1] == x[registerSource2]) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            // BNE (Branch if Not Equal)
            case 0b001: {
                if (x[registerSource1] != x[registerSource2]) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            // BLT (Branch if Less Than, signed)
            case 0b100: {
                if (static_cast<int64_t>(x[registerSource1]) < static_cast<int64_t>(x[registerSource2])) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            // BGE (Branch if Greater or Equal, signed)
            case 0b101: {
                if (static_cast<int64_t>(x[registerSource1]) >= static_cast<int64_t>(x[registerSource2])) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            // BLTU (Branch if Less Than Unsigned)
            case 0b110: {
                if (x[registerSource1] < x[registerSource2]) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            // BGEU (Branch if Greater or Equal Unsigned)
            case 0b111: {
                if (x[registerSource1] >= x[registerSource2]) {
                    uint64_t target = pc + signed_offset;
                    if ((target & 0b11) != 0) { return RaiseTrap(TrapCause::InstructionAddressMisaligned); }
                    pc = target;
                }
                else {
                    pc += 4;
                }
                return true;
            }

            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }

        // OP-IMM-32 (32-bit immediate instructions for RV64)
        case 0x1B: {
            uint64_t offset = (code >> 20) & 0x1F;
            switch(funct3) {
                // ADDIW (Add Immediate Word)
                case 0b000: {
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(x[registerSource1] + numb));
                    }
                    pc += 4;
                    return true;
                }
                
                // SLLIW (Shift Left Logical Immediate Word)
                case 0b001: {
                    if (funct7 != 0x00) { 
                        return RaiseTrap(TrapCause::IllegalInstruction);
                    }
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(x[registerSource1]) << offset));
                    }
                    pc += 4;
                    return true;
                }

                // SRLIW / SRAIW
                case 0b101: {
                    switch(funct7) {
                    // SRLIW (Shift Right Logical Immediate Word)
                    case 0x00: {
                        if (registerDest != 0) {
                            x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(x[registerSource1]) >> offset));
                        }
                        pc += 4;
                        return true;
                    }

                    // SRAIW (Shift Right Arithmetic Immediate Word)
                    case 0x20: {
                        if (registerDest != 0) {
                            x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(x[registerSource1]) >> offset);
                        }
                        pc += 4;
                        return true;
                    }

                    default:
                        return RaiseTrap(TrapCause::IllegalInstruction);;
                    }
                }
            }
        }

        // OP-32 (32-bit register-register instructions for RV64)
        case 0x3B: {
            if (funct7 == 0x01) {
                switch(funct3) {
                case 0b000: {
                    if (registerDest != 0) {
                        x[registerDest] = ((int32_t)x[registerSource1] * (int32_t)x[registerSource2]);
                    }
                    pc += 4;
                    return true;
                }

                case 0b100: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = -1;
                        }
                        else if ((int32_t)x[registerSource1] == INT32_MIN && (int32_t)x[registerSource2] == -1) {
                            x[registerDest] = INT32_MIN;
                        }
                        else {
                            x[registerDest] = ((int32_t)x[registerSource1] / (int32_t)x[registerSource2]);
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b101: {
                    if (registerDest != 0) {
                        if ((uint32_t)x[registerSource2] == 0) {
                            x[registerDest] = -1;
                        }
                        else {
                            x[registerDest] = (int64_t)(int32_t)((uint32_t)x[registerSource1] / (uint32_t)x[registerSource2]);
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b110: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = x[registerSource1];
                        }
                        else if ((int32_t)x[registerSource1] == INT32_MIN && (int32_t)x[registerSource2] == -1) {
                            x[registerDest] = 0;
                        }
                        else {
                            x[registerDest] = (int32_t)x[registerSource1] % (int32_t)x[registerSource2];
                        }
                    }
                    pc += 4;
                    return true;
                }

                case 0b111: {
                    if (registerDest != 0) {
                        if (x[registerSource2] == 0) {
                            x[registerDest] = x[registerSource1];
                        }
                        else {
                            x[registerDest] = (int64_t)(int32_t)((uint32_t)x[registerSource1] % (uint32_t)x[registerSource2]);
                        }
                    }
                    pc += 4;
                    return true;
                }

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
            }

            switch(funct3) {
            // ADDW / SUBW
            case 0b000: {
                switch(funct7) {
                // ADDW (Add Word)
                case 0x00: {
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(x[registerSource1] + x[registerSource2]));
                    }
                    pc += 4;
                    return true;
                }

                // SUBW (Subtract Word)
                case 0x20: {
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(x[registerSource1] - x[registerSource2]));
                    }
                    pc += 4;
                    return true;
                }

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
            }
            
            // SLLW (Shift Left Logical Word)
            case 0b001: {
                if (funct7 != 0) {
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
                if (registerDest != 0) {
                    x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(x[registerSource1]) << (x[registerSource2] & 0x1F)));
                }
                pc += 4;
                return true;
            }

            // SRLW / SRAW
            case 0b101: {
                switch(funct7) {
                // SRLW (Shift Right Logical Word)
                case 0x00: {
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(x[registerSource1]) >> (x[registerSource2] & 0x1F)));
                    }
                    pc += 4;
                    return true;
                }

                // SRAW (Shift Right Arithmetic Word)
                case 0x20: {
                    if (registerDest != 0) {
                        x[registerDest] = static_cast<int64_t>(static_cast<int32_t>(x[registerSource1]) >> (x[registerSource2] & 0x1F));
                    }
                    pc += 4;
                    return true;
                }

                default:
                    return RaiseTrap(TrapCause::IllegalInstruction);
                }
            }
            
            default:
                return RaiseTrap(TrapCause::IllegalInstruction);
            }
        }
        
        default:
            return RaiseTrap(TrapCause::IllegalInstruction);
        }
    }

    uint64_t Run(uint64_t maxSteps) {
        for (uint64_t i = 0; i < maxSteps; ++i) {
            if (!Step()) {
                return 0;
            }
        }
        return pc;
    }
};

int main() {
    std::string fileName = "xv6-riscv/kernel/kernel";
    Bus bus(0x80000000, 128 * 1024 * 1024);
    RV64I cpu(bus);
    ElfFile elf(fileName);
    if (!elf.LoadElfBinary()) {
        std::cerr << "Cannot read ELF file: " << fileName << '\n';
        return 1;
    }
    if (!elf.ParseElf()) {
        std::cerr << "Invalid or unsupported ELF: " << fileName
               << " (expected little-endian RISC-V ET_EXEC)\n";
        return 2;
    }
    Loader loader(bus, elf);
    if (!loader.Load()) { return 3; }
    cpu.ResetCPU(elf.Entry());
    std::thread inputThread([&bus]() {
        char c;
        while (std::cin.get(c)) {
            bus.PushRx(static_cast<uint8_t>(c));
        }
    });
    inputThread.detach();
    while (cpu.Step()) {
    }
    return 0;
}