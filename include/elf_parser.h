#pragma once

#include <elf.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct ElfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Section {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t addralign = 0;
    uint64_t entsize = 0;
    uint32_t link = 0;
    uint32_t info = 0;
};

struct Symbol {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint16_t sectionIndex = 0;
    uint8_t binding = 0;
    uint8_t type = 0;
};

struct Relocation {
    uint64_t offset = 0;
    uint32_t type = 0;
    uint32_t symbolIndex = 0;
    int64_t addend = 0;
};

struct RelocationSection {
    size_t sectionIndex = 0;
    size_t targetSection = 0;
    std::vector<Relocation> entries;
};

struct ObjectFile {
    std::string path;
    std::vector<uint8_t> bytes;
    std::vector<Section> sections;
    size_t symtabIndex = 0;
    std::vector<Symbol> symbols;
    std::vector<RelocationSection> relocations;
};

ObjectFile loadObjectFile(const std::string& path);

std::vector<uint8_t> sectionContents(const ObjectFile& obj, size_t index);

std::string symbolDisplayName(const ObjectFile& obj, uint32_t symbolIndex);

std::string sectionTypeName(uint32_t type);
std::string symbolBindingName(uint8_t binding);
std::string symbolTypeName(uint8_t type);
std::string sectionIndexName(const ObjectFile& obj, uint16_t index);
std::string relocationTypeName(uint32_t type);

std::string fileName(const std::string& path);
std::string hex(uint64_t value);
std::string addendText(int64_t addend);
