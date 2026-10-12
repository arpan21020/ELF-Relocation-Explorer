#pragma once

#include "elf_parser.h"

#include <map>
#include <string>
#include <vector>

struct Definition {
    size_t file = 0;
    size_t symbol = 0;
};

struct SymbolTable {
    std::map<std::string, Definition> definitions;
    std::vector<std::string> errors;
    std::vector<std::string> notes;
};

SymbolTable buildSymbolTable(const std::vector<ObjectFile>& objects);

struct SymbolTarget {
    enum Kind { InSection, Absolute, WeakUndefined, Undefined, Common };
    Kind kind = Undefined;
    std::string name;
    size_t file = 0;
    uint16_t section = 0;
    uint64_t value = 0;
};

SymbolTarget resolveSymbol(const std::vector<ObjectFile>& objects, const SymbolTable& table,
                           size_t file, uint32_t symbolIndex);

struct RelocationRule {
    enum Range { Signed32, Unsigned32, Any64 };
    uint32_t type;
    bool pcRelative;
    unsigned fieldSize;
    Range range;
};

const RelocationRule* findRelocationRule(uint32_t type);
std::string formulaText(const RelocationRule& rule);
std::string rangeText(const RelocationRule& rule);
bool fitsField(const RelocationRule& rule, int64_t value);

bool printAnalysis(const std::vector<ObjectFile>& objects);
