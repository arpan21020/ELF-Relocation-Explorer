#include "elf_parser.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ElfError("cannot open file");
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

bool rangeFits(uint64_t offset, uint64_t size, uint64_t total) {
    return offset <= total && size <= total - offset;
}

template <typename T>
T readAt(const std::vector<uint8_t>& bytes, uint64_t offset, const std::string& what) {
    if (!rangeFits(offset, sizeof(T), bytes.size())) {
        throw ElfError(what + " lies outside the file (truncated or malformed)");
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::string readString(const ObjectFile& obj, size_t tableIndex, uint64_t index,
                       const std::string& what) {
    const Section& table = obj.sections[tableIndex];
    if (index >= table.size) {
        throw ElfError(what + " offset " + hex(index) + " is outside its string table");
    }
    const char* start = reinterpret_cast<const char*>(obj.bytes.data() + table.offset + index);
    size_t maxLength = table.size - index;
    size_t length = strnlen(start, maxLength);
    if (length == maxLength) {
        throw ElfError(what + " is not NUL-terminated");
    }
    return std::string(start, length);
}

Elf64_Ehdr readHeader(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < SELFMAG || std::memcmp(bytes.data(), ELFMAG, SELFMAG) != 0) {
        throw ElfError("not an ELF file (bad magic number)");
    }
    if (bytes.size() < EI_NIDENT) {
        throw ElfError("file is truncated inside the ELF identification bytes");
    }
    if (bytes[EI_CLASS] != ELFCLASS64) {
        throw ElfError("unsupported ELF class (only ELF64 is supported)");
    }
    if (bytes[EI_DATA] != ELFDATA2LSB) {
        throw ElfError("unsupported byte order (only little-endian is supported)");
    }
    if (bytes[EI_VERSION] != EV_CURRENT) {
        throw ElfError("unsupported ELF version");
    }

    Elf64_Ehdr header = readAt<Elf64_Ehdr>(bytes, 0, "ELF header");
    if (header.e_type != ET_REL) {
        throw ElfError("unsupported file type " + std::to_string(header.e_type) +
                       " (only relocatable object files, ET_REL, are supported)");
    }
    if (header.e_machine != EM_X86_64) {
        throw ElfError("unsupported machine " + std::to_string(header.e_machine) +
                       " (only x86-64 is supported)");
    }
    return header;
}

void readSections(ObjectFile& obj, const Elf64_Ehdr& header) {
    if (header.e_shoff == 0 || header.e_shnum == 0) {
        throw ElfError("no section header table (extended section numbering is not supported)");
    }
    if (header.e_shentsize != sizeof(Elf64_Shdr)) {
        throw ElfError("unexpected section header entry size " + std::to_string(header.e_shentsize));
    }
    if (!rangeFits(header.e_shoff, uint64_t(header.e_shnum) * sizeof(Elf64_Shdr), obj.bytes.size())) {
        throw ElfError("section header table lies outside the file");
    }
    if (header.e_shstrndx == SHN_UNDEF || header.e_shstrndx >= header.e_shnum) {
        throw ElfError("invalid section name string table index");
    }

    std::vector<uint32_t> nameOffsets;
    for (size_t i = 0; i < header.e_shnum; ++i) {
        auto raw = readAt<Elf64_Shdr>(obj.bytes, header.e_shoff + i * sizeof(Elf64_Shdr),
                                      "section header " + std::to_string(i));
        if (raw.sh_type != SHT_NOBITS && !rangeFits(raw.sh_offset, raw.sh_size, obj.bytes.size())) {
            throw ElfError("contents of section " + std::to_string(i) + " lie outside the file");
        }
        Section section;
        section.type = raw.sh_type;
        section.flags = raw.sh_flags;
        section.offset = raw.sh_offset;
        section.size = raw.sh_size;
        section.addralign = raw.sh_addralign;
        section.entsize = raw.sh_entsize;
        section.link = raw.sh_link;
        section.info = raw.sh_info;
        obj.sections.push_back(section);
        nameOffsets.push_back(raw.sh_name);
    }

    if (obj.sections[header.e_shstrndx].type != SHT_STRTAB) {
        throw ElfError("section name string table is not of type SHT_STRTAB");
    }
    for (size_t i = 0; i < obj.sections.size(); ++i) {
        obj.sections[i].name = readString(obj, header.e_shstrndx, nameOffsets[i], "section name");
    }
}

void checkTableShape(const Section& section, uint64_t entrySize) {
    if (section.entsize != entrySize || section.size % entrySize != 0) {
        throw ElfError("section " + section.name + " has an unexpected entry size");
    }
}

void checkSymbolSection(const ObjectFile& obj, const Symbol& symbol, size_t index) {
    uint16_t shndx = symbol.sectionIndex;
    bool special = shndx == SHN_UNDEF || shndx == SHN_ABS || shndx == SHN_COMMON;
    if (special) {
        return;
    }
    if (shndx >= SHN_LORESERVE) {
        throw ElfError("symbol " + std::to_string(index) + " uses unsupported special section index " +
                       hex(shndx));
    }
    if (shndx >= obj.sections.size()) {
        throw ElfError("symbol " + std::to_string(index) + " refers to nonexistent section " +
                       std::to_string(shndx));
    }
}

void readSymbols(ObjectFile& obj) {
    for (size_t i = 0; i < obj.sections.size(); ++i) {
        if (obj.sections[i].type == SHT_SYMTAB) {
            if (obj.symtabIndex != 0) {
                throw ElfError("more than one symbol table is not supported");
            }
            obj.symtabIndex = i;
        }
    }
    if (obj.symtabIndex == 0) {
        return;
    }

    const Section& symtab = obj.sections[obj.symtabIndex];
    checkTableShape(symtab, sizeof(Elf64_Sym));
    if (symtab.link == 0 || symtab.link >= obj.sections.size() ||
        obj.sections[symtab.link].type != SHT_STRTAB) {
        throw ElfError("symbol table does not link to a valid string table");
    }

    size_t count = symtab.size / sizeof(Elf64_Sym);
    for (size_t i = 0; i < count; ++i) {
        auto raw = readAt<Elf64_Sym>(obj.bytes, symtab.offset + i * sizeof(Elf64_Sym), "symbol");
        Symbol symbol;
        symbol.name = readString(obj, symtab.link, raw.st_name, "symbol name");
        symbol.value = raw.st_value;
        symbol.size = raw.st_size;
        symbol.sectionIndex = raw.st_shndx;
        symbol.binding = ELF64_ST_BIND(raw.st_info);
        symbol.type = ELF64_ST_TYPE(raw.st_info);
        checkSymbolSection(obj, symbol, i);
        obj.symbols.push_back(symbol);
    }
}

void readRelocationSection(ObjectFile& obj, size_t index) {
    const Section& section = obj.sections[index];
    checkTableShape(section, sizeof(Elf64_Rela));
    if (obj.symtabIndex == 0 || section.link != obj.symtabIndex) {
        throw ElfError("relocation section " + section.name + " does not link to the symbol table");
    }
    if (section.info == 0 || section.info >= obj.sections.size()) {
        throw ElfError("relocation section " + section.name + " has an invalid target section");
    }

    RelocationSection rela;
    rela.sectionIndex = index;
    rela.targetSection = section.info;
    size_t count = section.size / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
        auto raw = readAt<Elf64_Rela>(obj.bytes, section.offset + i * sizeof(Elf64_Rela), "relocation");
        Relocation reloc;
        reloc.offset = raw.r_offset;
        reloc.type = ELF64_R_TYPE(raw.r_info);
        reloc.symbolIndex = ELF64_R_SYM(raw.r_info);
        reloc.addend = raw.r_addend;
        if (reloc.symbolIndex >= obj.symbols.size()) {
            throw ElfError("relocation in " + section.name + " refers to nonexistent symbol " +
                           std::to_string(reloc.symbolIndex));
        }
        rela.entries.push_back(reloc);
    }
    obj.relocations.push_back(rela);
}

void readRelocations(ObjectFile& obj) {
    for (size_t i = 0; i < obj.sections.size(); ++i) {
        if (obj.sections[i].type == SHT_REL) {
            throw ElfError("SHT_REL sections (implicit addends) are not supported; x86-64 uses SHT_RELA");
        }
        if (obj.sections[i].type == SHT_RELA) {
            readRelocationSection(obj, i);
        }
    }
}

}

ObjectFile loadObjectFile(const std::string& path) {
    try {
        ObjectFile obj;
        obj.path = path;
        obj.bytes = readFile(path);
        Elf64_Ehdr header = readHeader(obj.bytes);
        readSections(obj, header);
        readSymbols(obj);
        readRelocations(obj);
        return obj;
    } catch (const ElfError& e) {
        throw ElfError(path + ": " + e.what());
    }
}

std::vector<uint8_t> sectionContents(const ObjectFile& obj, size_t index) {
    const Section& section = obj.sections[index];
    if (section.type == SHT_NOBITS) {
        return {};
    }
    auto begin = obj.bytes.begin() + static_cast<std::ptrdiff_t>(section.offset);
    return std::vector<uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(section.size));
}

std::string symbolDisplayName(const ObjectFile& obj, uint32_t symbolIndex) {
    const Symbol& symbol = obj.symbols[symbolIndex];
    if (symbol.type == STT_SECTION && symbol.sectionIndex < obj.sections.size()) {
        return obj.sections[symbol.sectionIndex].name;
    }
    return symbol.name.empty() ? "(no name)" : symbol.name;
}

std::string sectionTypeName(uint32_t type) {
    switch (type) {
        case SHT_NULL: return "NULL";
        case SHT_PROGBITS: return "PROGBITS";
        case SHT_SYMTAB: return "SYMTAB";
        case SHT_STRTAB: return "STRTAB";
        case SHT_RELA: return "RELA";
        case SHT_NOBITS: return "NOBITS";
        case SHT_NOTE: return "NOTE";
        case SHT_REL: return "REL";
        case SHT_GROUP: return "GROUP";
        case SHT_INIT_ARRAY: return "INIT_ARRAY";
        case SHT_FINI_ARRAY: return "FINI_ARRAY";
        case SHT_X86_64_UNWIND: return "X86_64_UNWIND";
        default: return hex(type);
    }
}

std::string symbolBindingName(uint8_t binding) {
    switch (binding) {
        case STB_LOCAL: return "LOCAL";
        case STB_GLOBAL: return "GLOBAL";
        case STB_WEAK: return "WEAK";
        default: return std::to_string(binding);
    }
}

std::string symbolTypeName(uint8_t type) {
    switch (type) {
        case STT_NOTYPE: return "NOTYPE";
        case STT_OBJECT: return "OBJECT";
        case STT_FUNC: return "FUNC";
        case STT_SECTION: return "SECTION";
        case STT_FILE: return "FILE";
        case STT_COMMON: return "COMMON";
        case STT_TLS: return "TLS";
        default: return std::to_string(type);
    }
}

std::string sectionIndexName(const ObjectFile& obj, uint16_t index) {
    switch (index) {
        case SHN_UNDEF: return "UND";
        case SHN_ABS: return "ABS";
        case SHN_COMMON: return "COMMON";
        default: return index < obj.sections.size() ? obj.sections[index].name : hex(index);
    }
}

std::string relocationTypeName(uint32_t type) {
    switch (type) {
        case R_X86_64_NONE: return "R_X86_64_NONE";
        case R_X86_64_64: return "R_X86_64_64";
        case R_X86_64_PC32: return "R_X86_64_PC32";
        case R_X86_64_GOT32: return "R_X86_64_GOT32";
        case R_X86_64_PLT32: return "R_X86_64_PLT32";
        case R_X86_64_GOTPCREL: return "R_X86_64_GOTPCREL";
        case R_X86_64_32: return "R_X86_64_32";
        case R_X86_64_32S: return "R_X86_64_32S";
        case R_X86_64_16: return "R_X86_64_16";
        case R_X86_64_PC16: return "R_X86_64_PC16";
        case R_X86_64_8: return "R_X86_64_8";
        case R_X86_64_PC8: return "R_X86_64_PC8";
        case R_X86_64_TPOFF32: return "R_X86_64_TPOFF32";
        case R_X86_64_TLSGD: return "R_X86_64_TLSGD";
        case R_X86_64_TLSLD: return "R_X86_64_TLSLD";
        case R_X86_64_DTPOFF32: return "R_X86_64_DTPOFF32";
        case R_X86_64_GOTTPOFF: return "R_X86_64_GOTTPOFF";
        case R_X86_64_PC64: return "R_X86_64_PC64";
        case R_X86_64_GOTPC32: return "R_X86_64_GOTPC32";
        case R_X86_64_GOTPCRELX: return "R_X86_64_GOTPCRELX";
        case R_X86_64_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
        default: return "R_X86_64_<" + std::to_string(type) + ">";
    }
}

std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string hex(uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string addendText(int64_t addend) {
    return addend < 0 ? "- " + hex(0 - static_cast<uint64_t>(addend)) : "+ " + hex(static_cast<uint64_t>(addend));
}
