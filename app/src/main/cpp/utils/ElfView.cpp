//
// Created by kinit on 2021-10-25.
//
// This file(ElfView.cpp) is licensed under the Apache License 2.0.
// reference: https://flapenguin.me/elf-dt-gnu-hash

#include <cstring>
#include <type_traits>
#include <optional>
#include <limits>
#include <unordered_map>

#include <elf.h>

#include <fmt/format.h>

#include "utils/Log.h"

#include "ElfView.h"
#include "debug_utils.h"

#ifndef SHT_GNU_HASH
#define SHT_GNU_HASH      0x6ffffff6
#endif

// for mmkv::KeyHasher, mmkv::KeyEqualer
#include "MMKV.h"

#include "xz_decoder.h"

static auto constexpr LOG_TAG = "ElfView";

using namespace utils;

static inline auto constexpr kElf32 = ElfView::ElfClass::kElf32;
static inline auto constexpr kElf64 = ElfView::ElfClass::kElf64;

class ElfView::ElfInfo {
public:
    // prevent accidental copy
    ElfInfo() = default;
    ElfInfo(const ElfInfo&) = delete;
    ElfInfo& operator=(const ElfInfo&) = delete;

    ElfClass elfClass = ElfClass::kNone;
    uint16_t machine = 0;
    std::string soname;
    // the p_vaddr of the first PT_LOAD segment in ELF **file**
    uint64_t loadBias = 0;
    size_t loadedSize = 0;
    const void* sysv_hash = nullptr;
    uint32_t sysv_hash_nbucket = 0;
    uint32_t sysv_hash_nchain = 0;
    const uint32_t* sysv_hash_bucket = nullptr;
    const uint32_t* sysv_hash_chain = nullptr;
    const void* gnu_hash = nullptr;
    const void* symtab = nullptr;
    size_t symtab_size = 0;
    const char* strtab = nullptr;
    const void* dynsym = nullptr;
    size_t dynsym_size = 0;
    const char* dynstr = nullptr;
    bool use_rela = false;
    const void* reldyn = nullptr;
    size_t reldyn_size = 0;
    const void* reladyn = nullptr;
    size_t reladyn_size = 0;
    const void* relplt = nullptr;
    size_t relplt_size = 0;
    std::span<const uint8_t> miniDebugInfo;
    // the offset value is saved as is here, must subtract loadBias to get the real offset
    // symbol from compressed ".gnu_debugdata", aka MiniDebugInfo
    std::unordered_map<std::string, uint64_t, mmkv::KeyHasher, mmkv::KeyEqualer> compressedDebugSymbols;
};

ElfView::ElfView() {
    mElfInfo = std::make_unique<ElfInfo>();
}

ElfView::~ElfView() noexcept = default;

using ElfInfo = ElfView::ElfInfo;
using ElfClass = ElfView::ElfClass;

bool ElfView::IsValid() const noexcept {
    return !mMemory.empty() && mElfInfo && mElfInfo->elfClass != ElfClass::kNone;
}

void ElfView::Detach() noexcept {
    mMemory = {};
    mElfInfo = nullptr;
    mIsLoaded = false;
}

int ElfView::GetPointerSize() const noexcept {
    if (!IsValid()) {
        return 0;
    }
    switch (mElfInfo->elfClass) {
        case ElfClass::kElf32:
            return 4;
        case ElfClass::kElf64:
            return 8;
        default:
            return 0;
    }
}

const ElfInfo& ElfView::GetElfInfo() const noexcept {
    return *mElfInfo;
}

int ElfView::GetArchitecture() const noexcept {
    if (!IsValid()) {
        return 0;
    }
    return mElfInfo->machine;
}

/**
 * Get the load bias of the elf file. Typically, you don't need to use this value.
 * @return the load bias of the elf file.
 */
uint64_t ElfView::GetLoadBias() const noexcept {
    return mElfInfo->loadBias;
}

size_t ElfView::GetLoadedSize() const noexcept {
    return mElfInfo->loadedSize;
}

/**
 * Get the soname of the elf file.
 * @return may be empty string.
 */
const std::string& ElfView::GetSoname() const noexcept {
    return mElfInfo->soname;
}

void ElfView::ParseMiniDebugInfo(std::span<const uint8_t> input) {
    // check xz magic
    if (input.size() < 6 || input[0] != 0xFD || input[1] != '7' || input[2] != 'z' || input[3] != 'X' || input[4] != 'Z') {
        return;
    }
    bool isSuccess = false;
    std::string errorMsg;
    const auto decompressed = util::DecodeXzData(input, &isSuccess, &errorMsg);
    if (decompressed.empty() || !isSuccess) {
        LOGW("Failed to decompress mini debug info: {}", errorMsg);
        return;
    }
    ParseDebugSymbol(decompressed);
}

template<ElfClass kElfClass>
static void InitElfInfo(std::span<const uint8_t> file, ElfInfo& info, bool isLoaded) {
    using Elf_Ehdr = std::conditional_t<kElfClass == kElf32, Elf32_Ehdr, Elf64_Ehdr>;
    using Elf_Phdr = std::conditional_t<kElfClass == kElf32, Elf32_Phdr, Elf64_Phdr>;
    using Elf_Dyn = std::conditional_t<kElfClass == kElf32, Elf32_Dyn, Elf64_Dyn>;
    using Elf_Shdr = std::conditional_t<kElfClass == kElf32, Elf32_Shdr, Elf64_Shdr>;
    using Elf_Sym = std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>;
    using Elf_Rel = std::conditional_t<kElfClass == kElf32, Elf32_Rel, Elf64_Rel>;
    using Elf_Rela = std::conditional_t<kElfClass == kElf32, Elf32_Rela, Elf64_Rela>;
    info.machine = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_machine;
    // walk through program header
    auto phoff = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_phoff;
    if (phoff != 0) {
        auto phnum = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_phnum;
        auto phentsize = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_phentsize;
        uint64_t firstLoadedSegmentStart = std::numeric_limits<uint64_t>::max();
        uint64_t lastLoadedSegmentEnd = 0;
        const Elf_Phdr* phdrSelf = nullptr;
        const Elf_Phdr* phdrDynamic = nullptr;
        for (int i = 0; i < phnum; i++) {
            const auto* phdr = reinterpret_cast<const Elf_Phdr*>(file.data() + phoff + i * phentsize);
            if (phdr->p_type == PT_PHDR) {
                phdrSelf = phdr;
            } else if (phdr->p_type == PT_DYNAMIC) {
                phdrDynamic = phdr;
            } else if (phdr->p_type == PT_LOAD) {
                if (phdr->p_vaddr < firstLoadedSegmentStart) {
                    firstLoadedSegmentStart = phdr->p_vaddr;
                }
                if (phdr->p_vaddr + phdr->p_memsz > lastLoadedSegmentEnd) {
                    lastLoadedSegmentEnd = phdr->p_vaddr + phdr->p_memsz;
                }
            }
        }
        info.loadBias = firstLoadedSegmentStart;
        info.loadedSize = lastLoadedSegmentEnd - firstLoadedSegmentStart;
        if (phdrDynamic != nullptr) {
            // walk through dynamic section
            uint64_t sonameOffset = 0;
            const char* strtab = nullptr;
            for (int i = 0; i < phdrDynamic->p_memsz / sizeof(Elf_Dyn); i++) {
                auto offset = (isLoaded ? phdrDynamic->p_vaddr : phdrDynamic->p_offset) + i * sizeof(Elf_Dyn);
                const auto* dyn = reinterpret_cast<const Elf_Dyn*>(file.data() + offset);
                switch (dyn->d_tag) {
                    case DT_NULL: {
                        break;
                    }
                    case DT_SONAME: {
                        sonameOffset = dyn->d_un.d_val;
                        break;
                    }
                    case DT_STRTAB: {
                        strtab = reinterpret_cast<const char*>(file.data() + dyn->d_un.d_val);
                        break;
                    }
                    case DT_PLTREL: {
                        info.use_rela = dyn->d_un.d_val == DT_RELA;
                        break;
                    }
                    case DT_REL: {
                        info.reldyn = reinterpret_cast<const Elf_Rel*>(file.data() + dyn->d_un.d_ptr);
                        break;
                    }
                    case DT_RELA: {
                        info.reladyn = reinterpret_cast<const Elf_Rela*>(file.data() + dyn->d_un.d_ptr);
                        break;
                    }
                    case DT_RELSZ: {
                        info.reldyn_size = dyn->d_un.d_val / sizeof(Elf_Rel);
                        break;
                    }
                    case DT_RELASZ: {
                        info.reladyn_size = dyn->d_un.d_val / sizeof(Elf_Rela);
                        break;
                    }
                    case DT_JMPREL: {
                        info.relplt = reinterpret_cast<const Elf_Rel*>(file.data() + dyn->d_un.d_ptr);
                        break;
                    }
                    case DT_PLTRELSZ: {
                        info.relplt_size = dyn->d_un.d_val / sizeof(Elf_Rel);
                        break;
                    }
                    default: {
                        // ignore
                        break;
                    }
                }
            }
            if (sonameOffset != 0 && strtab != nullptr) {
                info.soname = strtab + sonameOffset;
            }
        }
    }
    // walk through section header
    auto shoff = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_shoff;
    if (shoff == 0) {
        return;
    }
    const auto* ehdr = reinterpret_cast<const Elf_Ehdr*>(file.data());
    auto shnum = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_shnum;
    auto shentsize = reinterpret_cast<const Elf_Ehdr*>(file.data())->e_shentsize;
    const char* sectionHeaderStringTable = [&]() {
        auto* shstrtab = reinterpret_cast<const Elf_Shdr*>(file.data() + shoff + ehdr->e_shstrndx * shentsize);
        return reinterpret_cast<const char*>(file.data() + (isLoaded ? shstrtab->sh_addr : shstrtab->sh_offset));
    }();
    for (int i = 0; i < shnum; i++) {
        const auto* shdr = reinterpret_cast<const Elf_Shdr*>(file.data() + shoff + i * shentsize);
        const char* name = sectionHeaderStringTable + shdr->sh_name;
        switch (shdr->sh_type) {
            case SHT_STRTAB: {
                if (strcmp(name, ".dynstr") == 0) {
                    info.dynstr = reinterpret_cast<const char*>(file.data() + (isLoaded ? shdr->sh_addr : shdr->sh_offset));
                } else if (strcmp(name, ".strtab") == 0) {
                    info.strtab = reinterpret_cast<const char*>(file.data() + (isLoaded ? shdr->sh_addr : shdr->sh_offset));
                }
                break;
            }
            case SHT_SYMTAB: {
                if (strcmp(name, ".symtab") == 0) {
                    info.symtab = file.data() + shdr->sh_offset;
                    info.symtab_size = shdr->sh_size / sizeof(Elf_Sym);
                }
                break;
            }
            case SHT_DYNSYM: {
                info.dynsym = file.data() + (isLoaded ? shdr->sh_addr : shdr->sh_offset);
                info.dynsym_size = shdr->sh_size / sizeof(Elf_Sym);
                break;
            }
            case SHT_HASH: {
                info.sysv_hash = file.data() + (isLoaded ? shdr->sh_addr : shdr->sh_offset);
                const auto* rawdata = reinterpret_cast<const uint32_t*>(file.data() + shdr->sh_offset);
                info.sysv_hash_nbucket = rawdata[0];
                info.sysv_hash_nchain = rawdata[1];
                info.sysv_hash_bucket = rawdata + 2;
                info.sysv_hash_chain = info.sysv_hash_bucket + info.sysv_hash_nbucket;
                break;
            }
            case SHT_GNU_HASH: {
                info.gnu_hash = file.data() + (isLoaded ? shdr->sh_addr : shdr->sh_offset);
                break;
            }
            case SHT_PROGBITS: {
                if (strcmp(name, ".gnu_debugdata") == 0) {
                    // mini debug info
                    if (!isLoaded) {
                        // obviously, debug data is not loaded, so only file(not memory) is supported
                        std::span<const uint8_t> debugData(file.data() + shdr->sh_offset, shdr->sh_size);
                        info.miniDebugInfo = debugData;
                    }
                }
                break;
            }
            default: {
                // ignore
                break;
            }
        }
    }
}


void ElfView::ParseDebugSymbol(std::span<const uint8_t> input) {
    // check elf magic
    if (input.size() < 64 || (memcmp(input.data(), ELFMAG, SELFMAG) != 0)) {
        return;
    }
    // walk through the elf file, and get the symbol table
    ElfInfo embedded;
    auto type = static_cast<ElfClass>(input[4]);
    embedded.elfClass = type;
    if (type == kElf32) {
        InitElfInfo<kElf32>(input, embedded, false);
    } else if (type == kElf64) {
        InitElfInfo<kElf64>(input, embedded, false);
    }
    LOGD("input size: {}, symtab size: {}", input.size(), embedded.symtab_size);
    // walk through the symbol table, and add it to the symbol map
    // finally release the memory, because debug symbol is typically large (and it is not file-backed)
    if (embedded.elfClass == kElf32) {
        const auto* symtab = static_cast<const Elf32_Sym*>(embedded.symtab);
        for (uint32_t i = 0; i < embedded.symtab_size; i++) {
            const char* symname = embedded.strtab + symtab[i].st_name;
            if (symname[0] == '\0') {
                continue;
            }
            mElfInfo->compressedDebugSymbols.emplace(symname, symtab[i].st_value);
        }
    } else if (embedded.elfClass == kElf64) {
        const auto* symtab = static_cast<const Elf64_Sym*>(embedded.symtab);
        for (uint32_t i = 0; i < embedded.symtab_size; i++) {
            const char* symname = embedded.strtab + symtab[i].st_name;
            if (symname[0] == '\0') {
                continue;
            }
            mElfInfo->compressedDebugSymbols.emplace(symname, symtab[i].st_value);
        }
    }
}

void ElfView::AttachFileMemMapping(std::span<const uint8_t> fileMap) noexcept {
    mMemory = fileMap;
    mIsLoaded = false;
    mElfInfo = std::make_unique<ElfInfo>();
    if (mMemory.data() == nullptr || mMemory.size() < 64 || (memcmp(mMemory.data(), ELFMAG, SELFMAG) != 0)) {
        // invalid elf, ignore
    } else {
        auto& elfInfo = *mElfInfo;
        auto type = static_cast<ElfClass>(mMemory[4]);
        elfInfo.elfClass = type;
        if (type == kElf32) {
            InitElfInfo<kElf32>(mMemory, elfInfo, mIsLoaded);
        } else if (type == kElf64) {
            InitElfInfo<kElf64>(mMemory, elfInfo, mIsLoaded);
        }
        // if we found mini debug info, parse it
        if (!elfInfo.miniDebugInfo.empty()) {
            ParseMiniDebugInfo(elfInfo.miniDebugInfo);
        }
    }
}

void ElfView::AttachLoadedMemoryView(std::span<const uint8_t> memory) {
    mMemory = memory;
    mIsLoaded = true;
    mElfInfo = std::make_unique<ElfInfo>();
    if (mMemory.data() == nullptr || mMemory.size() < 64 || (memcmp(mMemory.data(), ELFMAG, SELFMAG) != 0)) {
        // invalid elf, ignore
    } else {
        auto& elfInfo = *mElfInfo;
        auto type = static_cast<ElfClass>(mMemory[4]);
        elfInfo.elfClass = type;
        if (type == kElf32) {
            InitElfInfo<kElf32>(mMemory, elfInfo, mIsLoaded);
        } else if (type == kElf64) {
            InitElfInfo<kElf64>(mMemory, elfInfo, mIsLoaded);
        }
    }
}

// SysV hash
static inline uint32_t elf_sysv_hash(std::string_view name) {
    uint32_t h = 0, g;
    int i = 0;
    while (i < name.size()) {
        h = (h << 4) + *(name.data() + (i++));
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

// GNU hash
static inline uint32_t elf_gnu_hash(std::string_view name) {
    uint32_t h = 5381;
    int i = 0;
    while (i < name.size()) {
        h += (h << 5) + *(name.data() + (i++));
    }
    return h;
}

template<ElfClass kElfClass>
[[nodiscard]] static bool GetDynamicSymbolIndexImpl(std::span<const uint8_t> file, const ElfInfo& info,
                                                    std::string_view symbol, uint32_t& outIndex,
                                                    const std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>** outSymbol,
                                                    bool searchForUndefined = false) {
    using Elf_Sym = std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>;
    if (info.elfClass == ElfClass::kNone || file.empty()) {
        return false;
    }
    if (info.dynsym == nullptr || info.dynstr == nullptr) {
        return false;
    }
    if (!searchForUndefined) {
        if (info.gnu_hash != nullptr) {
            using bloom_el_t = std::conditional_t<kElfClass == kElf32, uint32_t, uint64_t>;
            constexpr uint32_t ELFCLASS_BITS = kElfClass == kElf32 ? 32 : 64;
            const auto* hashtab = static_cast<const uint32_t*>(info.gnu_hash);
            const auto symbol_hash = elf_gnu_hash(symbol);
            const auto nbuckets = hashtab[0];
            const auto symoffset = hashtab[1];
            const auto bloom_size = hashtab[2];
            const auto bloom_shift = hashtab[3];
            const auto* bloom = reinterpret_cast<const bloom_el_t*>(&hashtab[4]);
            const auto* buckets = reinterpret_cast<const uint32_t*>(&bloom[bloom_size]);
            const auto* chain = &buckets[nbuckets];
            bloom_el_t word = bloom[(symbol_hash / ELFCLASS_BITS) % bloom_size];
            bloom_el_t mask = (bloom_el_t) 1 << (symbol_hash % ELFCLASS_BITS)
                    | (bloom_el_t) 1 << ((symbol_hash >> bloom_shift) % ELFCLASS_BITS);
            // If at least one bit is not set, a symbol is surely missing.
            if ((word & mask) != mask) {
                return false;
            }
            uint32_t symix = buckets[symbol_hash % nbuckets];
            if (symix < symoffset) {
                return false;
            }
            const auto* dynsym = static_cast<const Elf_Sym*>(info.dynsym);
            // Loop through the chain.
            while (true) {
                const char* symname = info.dynstr + dynsym[symix].st_name;
                const uint32_t hash = chain[symix - symoffset];
                if ((symbol_hash | 1) == (hash | 1) && symbol == symname) {
                    const Elf_Sym& sym = dynsym[symix];
                    // found.
                    outIndex = symix;
                    *outSymbol = &sym;
                    return true;
                }
                // Chain ends with an element with the lowest bit set to 1.
                if (hash & 1) {
                    break;
                }
                symix++;
            }
        }
        if (info.sysv_hash != nullptr) {
            const Elf_Sym* target = nullptr;
            uint32_t hash = elf_sysv_hash(symbol);
            uint32_t index = info.sysv_hash_bucket[hash % info.sysv_hash_nbucket];
            const auto* sym = static_cast<const Elf_Sym*>(info.dynsym);
            if (symbol == (info.dynstr + sym[index].st_name)) {
                // found.
                target = sym + index;
            }
            if (!target) {
                do {
                    index = info.sysv_hash_chain[index];
                    if (symbol == (info.dynstr + sym[index].st_name)) {
                        // found.
                        target = sym + index;
                        break;
                    }
                } while (index != 0);
            }
            if (target) {
                outIndex = index;
                *outSymbol = target;
                return true;
            }
        }
    }
    // if still not found, search the whole dynsym
    const auto* symtab = static_cast<const Elf_Sym*>(info.dynsym);
    for (uint32_t i = 0; i < info.dynsym_size; i++) {
        const char* symname = info.dynstr + symtab[i].st_name;
        if (strncmp(symname, symbol.data(), symbol.size()) == 0 && symname[symbol.size()] == '\0') {
            outIndex = i;
            *outSymbol = &symtab[i];
            return true;
        }
    }
    return false;
}

template<ElfClass kElfClass>
[[nodiscard]] static const std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>*
GetNonDynamicSymbolImpl(std::span<const uint8_t> file, const ElfInfo& info, std::string_view symbol) {
    using Elf_Sym = std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>;
    if (info.elfClass == ElfClass::kNone || file.empty()) {
        return nullptr;
    }
    if (info.symtab != nullptr && info.strtab != nullptr) {
        const auto* symtab = static_cast<const Elf_Sym*>(info.symtab);
        for (uint32_t i = 0; i < info.symtab_size; i++) {
            const char* symname = info.strtab + symtab[i].st_name;
            if (symbol == symname) {
                return &symtab[i];
            }
        }
    }
    return nullptr;
}

uint64_t ElfView::GetSymbolOffset(std::string_view symbol) const {
    if (symbol.empty()) {
        return 0;
    }
    auto& elfInfo = *mElfInfo;
    if (elfInfo.elfClass == kElf32) {
        uint32_t index = 0;
        const Elf32_Sym* sym = nullptr;
        if (GetDynamicSymbolIndexImpl<kElf32>(mMemory, elfInfo, symbol, index, &sym)) {
            return sym->st_value - elfInfo.loadBias;
        }
    } else if (elfInfo.elfClass == kElf64) {
        uint32_t index = 0;
        const Elf64_Sym* sym = nullptr;
        if (GetDynamicSymbolIndexImpl<kElf64>(mMemory, elfInfo, symbol, index, &sym)) {
            return sym->st_value - elfInfo.loadBias;
        }
    }
    // search the symtab
    if (elfInfo.symtab != nullptr && elfInfo.strtab != nullptr) {
        if (elfInfo.elfClass == kElf32) {
            const auto* sym = GetNonDynamicSymbolImpl<kElf32>(mMemory, elfInfo, symbol);
            if (sym != nullptr) {
                return sym->st_value - elfInfo.loadBias;
            }
        } else if (elfInfo.elfClass == kElf64) {
            const auto* sym = GetNonDynamicSymbolImpl<kElf64>(mMemory, elfInfo, symbol);
            if (sym != nullptr) {
                return sym->st_value - elfInfo.loadBias;
            }
        }
    }
    // check the compressed debug symbols
    auto it = elfInfo.compressedDebugSymbols.find(std::string(symbol));
    if (it != elfInfo.compressedDebugSymbols.end()) {
        return it->second - elfInfo.loadBias;
    }
    return 0;
}

template<ElfClass kElfClass>
static const std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>*
GetFirstSymbolOffsetWithPrefixImpl(std::span<const uint8_t> file, const ElfInfo& info, std::string_view symbolPrefix) {
    using Elf_Sym = std::conditional_t<kElfClass == kElf32, Elf32_Sym, Elf64_Sym>;
    // since we only have the prefix, we have to search the whole dynsym and symtab
    if (info.dynsym != nullptr && info.dynstr != nullptr) {
        // search dynsym
        const auto* dynsym = static_cast<const Elf_Sym*>(info.dynsym);
        for (uint32_t i = 0; i < info.dynsym_size; i++) {
            const char* symname = info.dynstr + dynsym[i].st_name;
            if (strncmp(symname, symbolPrefix.data(), symbolPrefix.size()) == 0) {
                return &dynsym[i];
            }
        }
    }
    // then search symtab
    if (info.symtab != nullptr && info.strtab != nullptr) {
        const auto* symtab = static_cast<const Elf_Sym*>(info.symtab);
        for (uint32_t i = 0; i < info.symtab_size; i++) {
            const char* symname = info.strtab + symtab[i].st_name;
            if (strncmp(symname, symbolPrefix.data(), symbolPrefix.size()) == 0) {
                return &symtab[i];
            }
        }
    }
    // not found
    return nullptr;
}

[[nodiscard]] uint64_t ElfView::GetFirstSymbolOffsetWithPrefix(std::string_view symbolPrefix) const {
    if (symbolPrefix.empty() || !IsValid()) {
        return 0;
    }
    auto& elfInfo = *mElfInfo;
    if (elfInfo.elfClass == kElf32) {
        const auto* sym = GetFirstSymbolOffsetWithPrefixImpl<kElf32>(mMemory, elfInfo, symbolPrefix);
        if (sym != nullptr) {
            return sym->st_value - elfInfo.loadBias;
        }
    } else if (elfInfo.elfClass == kElf64) {
        const auto* sym = GetFirstSymbolOffsetWithPrefixImpl<kElf64>(mMemory, elfInfo, symbolPrefix);
        if (sym != nullptr) {
            return sym->st_value - elfInfo.loadBias;
        }
    }
    // walk through the compressed debug symbols
    for (const auto& [key, value]: elfInfo.compressedDebugSymbols) {
        if (key.starts_with(symbolPrefix)) {
            return value - elfInfo.loadBias;
        }
    }
    // not found
    return 0;
}

std::vector<uint64_t> ElfView::GetSymbolGotOffset(std::string_view symbol) const {
    if (symbol.empty()) {
        return {};
    }
    if (!IsValid()) {
        return {};
    }
    auto& elfInfo = *mElfInfo;
    ElfClass elfClass = elfInfo.elfClass;
    std::optional<uint32_t> dynSymIdx = std::nullopt;
    if (elfClass == kElf32) {
        uint32_t idx = -1;
        const Elf32_Sym* unused = nullptr;
        if (GetDynamicSymbolIndexImpl<kElf32>(mMemory, elfInfo, symbol, idx, &unused, true)) {
            dynSymIdx = idx;
        }
    } else if (elfClass == kElf64) {
        uint32_t idx = -1;
        const Elf64_Sym* unused = nullptr;
        if (GetDynamicSymbolIndexImpl<kElf64>(mMemory, elfInfo, symbol, idx, &unused, true)) {
            dynSymIdx = idx;
        }
    }
    if (!dynSymIdx.has_value()) {
        return {};
    }
    std::vector<uint64_t> result;
    const auto& info = elfInfo;
    auto symidx = dynSymIdx.value();
    if (elfClass == kElf32) {
        // ELF32
        if (info.use_rela) {
            for (int i = 0; i < info.relplt_size; i++) {
                const Elf32_Rela& rel = ((const Elf32_Rela*) info.relplt)[i];
                if (ELF32_R_SYM(rel.r_info) == symidx
                        && (ELF32_R_TYPE(rel.r_info) == R_ARM_JUMP_SLOT
                                || ELF32_R_TYPE(rel.r_info) == R_386_JMP_SLOT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                    break;
                }
            }
            for (int i = 0; i < info.reladyn_size; i++) {
                const Elf32_Rela& rel = ((const Elf32_Rela*) info.reladyn)[i];
                if (ELF32_R_SYM(rel.r_info) == symidx &&
                        (ELF32_R_TYPE(rel.r_info) == R_ARM_ABS32
                                || ELF32_R_TYPE(rel.r_info) == R_ARM_GLOB_DAT
                                || ELF32_R_TYPE(rel.r_info) == R_386_32
                                || ELF32_R_TYPE(rel.r_info) == R_386_GLOB_DAT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                }
            }
        } else {
            for (int i = 0; i < info.relplt_size; i++) {
                const Elf32_Rel& rel = ((const Elf32_Rel*) info.relplt)[i];
                if (ELF32_R_SYM(rel.r_info) == symidx
                        && (ELF32_R_TYPE(rel.r_info) == R_ARM_JUMP_SLOT
                                || ELF32_R_TYPE(rel.r_info) == R_386_JMP_SLOT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                    break;
                }
            }
            for (int i = 0; i < info.reldyn_size; i++) {
                const Elf32_Rel& rel = ((const Elf32_Rel*) info.reldyn)[i];
                if (ELF32_R_SYM(rel.r_info) == symidx &&
                        (ELF32_R_TYPE(rel.r_info) == R_ARM_ABS32
                                || ELF32_R_TYPE(rel.r_info) == R_ARM_GLOB_DAT
                                || ELF32_R_TYPE(rel.r_info) == R_386_32
                                || ELF32_R_TYPE(rel.r_info) == R_386_GLOB_DAT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                }
            }
            return result;
        }
    } else if (elfClass == kElf64) {
        // ELF64
        if (info.use_rela) {
            for (int i = 0; i < info.relplt_size; i++) {
                const Elf64_Rela& rel = ((const Elf64_Rela*) info.relplt)[i];
                if (ELF64_R_SYM(rel.r_info) == symidx
                        && (ELF64_R_TYPE(rel.r_info) == R_AARCH64_JUMP_SLOT
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_JUMP_SLOT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                    break;
                }
            }
            for (int i = 0; i < info.reladyn_size; i++) {
                const Elf64_Rela& rel = ((const Elf64_Rela*) info.reladyn)[i];
                if (ELF64_R_SYM(rel.r_info) == symidx &&
                        (ELF64_R_TYPE(rel.r_info) == R_AARCH64_ABS64
                                || ELF64_R_TYPE(rel.r_info) == R_AARCH64_GLOB_DAT
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_64
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_GLOB_DAT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                }
            }
        } else {
            for (int i = 0; i < info.relplt_size; i++) {
                const Elf64_Rel& rel = ((const Elf64_Rel*) info.relplt)[i];
                if (ELF64_R_SYM(rel.r_info) == symidx
                        && (ELF64_R_TYPE(rel.r_info) == R_AARCH64_JUMP_SLOT
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_JUMP_SLOT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                    break;
                }
            }
            for (int i = 0; i < info.reldyn_size; i++) {
                const Elf64_Rel& rel = ((const Elf64_Rel*) info.reldyn)[i];
                if (ELF64_R_SYM(rel.r_info) == symidx &&
                        (ELF64_R_TYPE(rel.r_info) == R_AARCH64_ABS64
                                || ELF64_R_TYPE(rel.r_info) == R_AARCH64_GLOB_DAT
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_64
                                || ELF64_R_TYPE(rel.r_info) == R_X86_64_GLOB_DAT)) {
                    result.emplace_back(uint64_t(rel.r_offset) - elfInfo.loadBias);
                }
            }
        }
    }
    return result;
}
