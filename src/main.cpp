#include "elf_parser.h"
#include "layout.h"
#include "relocation_analyzer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kExitUsage = 1;
constexpr int kExitBadInput = 2;
constexpr int kExitProblems = 3;

void printUsage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  elf-explorer inspect <file.o>...\n"
                 "  elf-explorer analyze <file.o>...\n"
                 "  elf-explorer relocate [--base ADDRESS] <file.o>...\n");
}

void printHeader(const ObjectFile& obj) {
    std::printf("File: %s\n", obj.path.c_str());
    std::printf("  Class: ELF64   Data: little-endian   Type: REL (relocatable object)   Machine: x86-64\n");
}

void printSections(const ObjectFile& obj) {
    std::printf("\nSections (%zu):\n", obj.sections.size());
    std::printf("  %4s  %-24s %-14s %10s %10s\n", "[Nr]", "Name", "Type", "Offset", "Size");
    for (size_t i = 0; i < obj.sections.size(); ++i) {
        const Section& section = obj.sections[i];
        std::printf("  [%2zu]  %-24s %-14s %10s %10s\n", i, section.name.c_str(),
                    sectionTypeName(section.type).c_str(), hex(section.offset).c_str(), hex(section.size).c_str());
    }
}

void printSymbols(const ObjectFile& obj) {
    if (obj.symtabIndex == 0) {
        std::printf("\nNo symbol table.\n");
        return;
    }
    std::printf("\nSymbols in %s (%zu):\n", obj.sections[obj.symtabIndex].name.c_str(), obj.symbols.size());
    std::printf("  %4s  %-24s %-7s %-8s %-14s %s\n", "Num", "Name", "Bind", "Type", "Section", "Value");
    for (size_t i = 0; i < obj.symbols.size(); ++i) {
        const Symbol& symbol = obj.symbols[i];
        std::printf("  %4zu  %-24s %-7s %-8s %-14s %s\n", i, symbolDisplayName(obj, uint32_t(i)).c_str(),
                    symbolBindingName(symbol.binding).c_str(), symbolTypeName(symbol.type).c_str(),
                    sectionIndexName(obj, symbol.sectionIndex).c_str(), hex(symbol.value).c_str());
    }
}

void printRelocations(const ObjectFile& obj) {
    for (const RelocationSection& rela : obj.relocations) {
        std::printf("\nRelocations in %s (patch %s, symbols from %s), %zu entries:\n",
                    obj.sections[rela.sectionIndex].name.c_str(), obj.sections[rela.targetSection].name.c_str(),
                    obj.sections[obj.symtabIndex].name.c_str(), rela.entries.size());
        std::printf("  %-10s %-24s %-24s %s\n", "Offset", "Type", "Symbol", "Addend");
        for (const Relocation& reloc : rela.entries) {
            std::printf("  %-10s %-24s %-24s %s\n", hex(reloc.offset).c_str(), relocationTypeName(reloc.type).c_str(),
                        symbolDisplayName(obj, reloc.symbolIndex).c_str(), addendText(reloc.addend).c_str());
        }
    }
    if (obj.relocations.empty()) {
        std::printf("\nNo relocations.\n");
    }
}

void inspect(const ObjectFile& obj) {
    printHeader(obj);
    printSections(obj);
    printSymbols(obj);
    printRelocations(obj);
}

bool parseAddress(const std::string& text, uint64_t& address) {
    try {
        size_t used = 0;
        address = std::stoull(text, &used, 0);
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage();
        return kExitUsage;
    }
    std::string command = argv[1];
    uint64_t base = 0x401000;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (command == "relocate" && arg == "--base") {
            if (i + 1 >= argc || !parseAddress(argv[i + 1], base)) {
                std::fprintf(stderr, "error: --base needs an address such as 0x401000\n");
                return kExitUsage;
            }
            ++i;
        } else {
            paths.push_back(arg);
        }
    }
    if ((command != "inspect" && command != "analyze" && command != "relocate") || paths.empty()) {
        printUsage();
        return kExitUsage;
    }

    std::vector<ObjectFile> objects;
    try {
        for (const std::string& path : paths) {
            objects.push_back(loadObjectFile(path));
        }
    } catch (const ElfError& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kExitBadInput;
    }

    if (command == "inspect") {
        for (size_t i = 0; i < objects.size(); ++i) {
            std::printf(i == 0 ? "" : "\n");
            inspect(objects[i]);
        }
        return 0;
    }
    bool ok = command == "analyze" ? printAnalysis(objects) : printRelocation(objects, base);
    return ok ? 0 : kExitProblems;
}
