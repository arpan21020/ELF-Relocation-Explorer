#include "relocation_analyzer.h"

#include <cinttypes>
#include <cstdio>

namespace {

const RelocationRule kRules[] = {
    {R_X86_64_PC32, true, 4, RelocationRule::Signed32},
    {R_X86_64_PLT32, true, 4, RelocationRule::Signed32},
    {R_X86_64_64, false, 8, RelocationRule::Any64},
    {R_X86_64_32, false, 4, RelocationRule::Unsigned32},
    {R_X86_64_32S, false, 4, RelocationRule::Signed32},
};

bool isGlobalDefinition(const Symbol& symbol) {
    bool visible = symbol.binding == STB_GLOBAL || symbol.binding == STB_WEAK;
    bool linkable = symbol.type != STT_SECTION && symbol.type != STT_FILE;
    return visible && linkable && symbol.sectionIndex != SHN_UNDEF;
}

bool isUndefinedReference(const Symbol& symbol) {
    return symbol.sectionIndex == SHN_UNDEF && symbol.binding != STB_LOCAL && !symbol.name.empty();
}

std::string definitionFile(const std::vector<ObjectFile>& objects, const Definition& def) {
    return fileName(objects[def.file].path);
}

void addDuplicate(const std::vector<ObjectFile>& objects, SymbolTable& table,
                  const std::string& name, const Definition& incoming) {
    Definition& current = table.definitions[name];
    bool currentWeak = objects[current.file].symbols[current.symbol].binding == STB_WEAK;
    bool incomingWeak = objects[incoming.file].symbols[incoming.symbol].binding == STB_WEAK;
    std::string currentFile = definitionFile(objects, current);
    std::string incomingFile = definitionFile(objects, incoming);

    if (!currentWeak && !incomingWeak) {
        table.errors.push_back("duplicate definition of '" + name + "': " + currentFile + " and " + incomingFile);
    } else if (currentWeak && !incomingWeak) {
        table.notes.push_back("strong '" + name + "' in " + incomingFile + " overrides weak definition in " +
                              currentFile);
        current = incoming;
    } else if (!currentWeak && incomingWeak) {
        table.notes.push_back("strong '" + name + "' in " + currentFile + " overrides weak definition in " +
                              incomingFile);
    } else {
        table.notes.push_back("'" + name + "' is weak in both " + currentFile + " and " + incomingFile +
                              "; using the first one (" + currentFile + ")");
    }
}

SymbolTarget targetFromDefinition(size_t file, const Symbol& symbol) {
    SymbolTarget target;
    target.file = file;
    target.value = symbol.value;
    if (symbol.sectionIndex == SHN_ABS) {
        target.kind = SymbolTarget::Absolute;
    } else if (symbol.sectionIndex == SHN_COMMON) {
        target.kind = SymbolTarget::Common;
    } else {
        target.kind = SymbolTarget::InSection;
        target.section = symbol.sectionIndex;
    }
    return target;
}

std::string describeTarget(const std::vector<ObjectFile>& objects, const SymbolTarget& target) {
    switch (target.kind) {
        case SymbolTarget::InSection: {
            const ObjectFile& obj = objects[target.file];
            std::string section = obj.sections[target.section].name;
            std::string text = "address of " + fileName(obj.path) + ":" + section + " + " + hex(target.value);
            return target.name == section ? text : text + "  (" + target.name + ")";
        }
        case SymbolTarget::Absolute:
            return "absolute value " + hex(target.value);
        case SymbolTarget::WeakUndefined:
            return "0  (weak symbol '" + target.name + "' is not defined anywhere)";
        case SymbolTarget::Common:
            return "unknown: '" + target.name + "' is a COMMON symbol, which this tool does not allocate";
        case SymbolTarget::Undefined:
            break;
    }
    return "unknown: '" + target.name + "' is not defined in any input file";
}

void printInputSummary(const std::vector<ObjectFile>& objects) {
    std::printf("== Input files ==\n\n");
    for (const ObjectFile& obj : objects) {
        size_t relocationCount = 0;
        for (const RelocationSection& rela : obj.relocations) {
            relocationCount += rela.entries.size();
        }
        std::printf("  %-16s %3zu sections, %3zu symbols, %3zu relocations\n", fileName(obj.path).c_str(),
                    obj.sections.size(), obj.symbols.size(), relocationCount);
    }
}

void printDefinitions(const std::vector<ObjectFile>& objects, const SymbolTable& table) {
    std::printf("Global definitions (the winning definition for each name):\n");
    std::printf("  %-20s %-7s %-16s %-14s %s\n", "Symbol", "Binding", "Defined in", "Section", "Value");
    for (const auto& [name, def] : table.definitions) {
        const ObjectFile& obj = objects[def.file];
        const Symbol& symbol = obj.symbols[def.symbol];
        std::printf("  %-20s %-7s %-16s %-14s %s\n", name.c_str(), symbolBindingName(symbol.binding).c_str(),
                    fileName(obj.path).c_str(), sectionIndexName(obj, symbol.sectionIndex).c_str(),
                    hex(symbol.value).c_str());
    }
    for (const std::string& note : table.notes) {
        std::printf("  note: %s\n", note.c_str());
    }
    for (const std::string& error : table.errors) {
        std::printf("  error: %s\n", error.c_str());
    }
}

size_t printReferences(const std::vector<ObjectFile>& objects, const SymbolTable& table) {
    std::printf("\nReferences to symbols defined in another file:\n");
    std::printf("  %-16s %-20s %-16s %-14s %-8s %s\n", "Referencing file", "Symbol", "Defined in", "Section",
                "Value", "Status");
    size_t references = 0;
    size_t unresolved = 0;
    for (const ObjectFile& obj : objects) {
        for (const Symbol& symbol : obj.symbols) {
            if (!isUndefinedReference(symbol)) {
                continue;
            }
            ++references;
            std::string fileText = fileName(obj.path);
            const char* from = fileText.c_str();
            auto found = table.definitions.find(symbol.name);
            if (found != table.definitions.end()) {
                const ObjectFile& definer = objects[found->second.file];
                const Symbol& def = definer.symbols[found->second.symbol];
                std::printf("  %-16s %-20s %-16s %-14s %-8s %s\n", from, symbol.name.c_str(),
                            fileName(definer.path).c_str(), sectionIndexName(definer, def.sectionIndex).c_str(),
                            hex(def.value).c_str(), "resolved");
            } else if (symbol.binding == STB_WEAK) {
                std::printf("  %-16s %-20s %-16s %-14s %-8s %s\n", from, symbol.name.c_str(), "-", "-", "-",
                            "weak, unresolved (address 0)");
            } else {
                ++unresolved;
                std::printf("  %-16s %-20s %-16s %-14s %-8s %s\n", from, symbol.name.c_str(), "-", "-", "-",
                            "UNRESOLVED");
            }
        }
    }
    if (references == 0) {
        std::printf("  (none)\n");
    }
    if (unresolved > 0) {
        std::printf("  error: %zu unresolved symbol reference(s)\n", unresolved);
    }
    return unresolved;
}

void printRelocationEntry(const std::vector<ObjectFile>& objects, const SymbolTable& table, size_t file,
                          const Section& target, const Relocation& reloc) {
    const ObjectFile& obj = objects[file];
    std::printf("  %s+%-6s %-16s %s %s\n", target.name.c_str(), hex(reloc.offset).c_str(),
                relocationTypeName(reloc.type).c_str(), symbolDisplayName(obj, reloc.symbolIndex).c_str(),
                addendText(reloc.addend).c_str());

    const RelocationRule* rule = findRelocationRule(reloc.type);
    if (rule == nullptr) {
        std::printf("      unsupported relocation type: not analyzed\n");
        return;
    }
    SymbolTarget symbol = resolveSymbol(objects, table, file, reloc.symbolIndex);
    std::printf("      D = %-11s (%u-byte field, must fit %s)\n", formulaText(*rule).c_str(), rule->fieldSize,
                rangeText(*rule).c_str());
    std::printf("      S = %s\n", describeTarget(objects, symbol).c_str());
    std::printf("      A = %" PRId64 "\n", reloc.addend);
    if (rule->pcRelative) {
        std::printf("      P = address of %s:%s + %s\n", fileName(obj.path).c_str(), target.name.c_str(),
                    hex(reloc.offset).c_str());
    }
    if (reloc.type == R_X86_64_PLT32) {
        std::printf("      PLT32 is computed like PC32 here: the definition is linked statically, so no PLT stub\n");
    }
}

void printRelocationAnalysis(const std::vector<ObjectFile>& objects, const SymbolTable& table) {
    std::printf("\n== Relocation analysis: which bytes must be patched, and how? ==\n\n");
    std::printf("In ET_REL files, symbol values and relocation offsets are section-relative.\n");
    std::printf("Final S and P depend on where the linker places each section, so they are\n");
    std::printf("described symbolically here. Run `relocate` to compute them under an assumed layout.\n");

    for (size_t file = 0; file < objects.size(); ++file) {
        const ObjectFile& obj = objects[file];
        for (const RelocationSection& rela : obj.relocations) {
            const Section& target = obj.sections[rela.targetSection];
            std::printf("\n%s  %s -> patches %s (%zu entries)\n", fileName(obj.path).c_str(),
                        obj.sections[rela.sectionIndex].name.c_str(), target.name.c_str(), rela.entries.size());
            for (const Relocation& reloc : rela.entries) {
                printRelocationEntry(objects, table, file, target, reloc);
            }
        }
    }
}

}

SymbolTable buildSymbolTable(const std::vector<ObjectFile>& objects) {
    SymbolTable table;
    for (size_t file = 0; file < objects.size(); ++file) {
        const std::vector<Symbol>& symbols = objects[file].symbols;
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (!isGlobalDefinition(symbols[i])) {
                continue;
            }
            Definition def{file, i};
            if (table.definitions.count(symbols[i].name) == 0) {
                table.definitions[symbols[i].name] = def;
            } else {
                addDuplicate(objects, table, symbols[i].name, def);
            }
        }
    }
    return table;
}

SymbolTarget resolveSymbol(const std::vector<ObjectFile>& objects, const SymbolTable& table, size_t file,
                           uint32_t symbolIndex) {
    const ObjectFile& obj = objects[file];
    const Symbol& symbol = obj.symbols[symbolIndex];

    SymbolTarget target;
    if (symbolIndex == 0) {
        target.kind = SymbolTarget::Absolute;
        target.name = "(no symbol)";
        return target;
    }
    if (symbol.binding == STB_LOCAL) {
        target = targetFromDefinition(file, symbol);
    } else {
        auto found = table.definitions.find(symbol.name);
        if (found == table.definitions.end()) {
            target.kind = symbol.binding == STB_WEAK ? SymbolTarget::WeakUndefined : SymbolTarget::Undefined;
        } else {
            const Definition& def = found->second;
            target = targetFromDefinition(def.file, objects[def.file].symbols[def.symbol]);
        }
    }
    target.name = symbolDisplayName(obj, symbolIndex);
    return target;
}

const RelocationRule* findRelocationRule(uint32_t type) {
    for (const RelocationRule& rule : kRules) {
        if (rule.type == type) {
            return &rule;
        }
    }
    return nullptr;
}

std::string formulaText(const RelocationRule& rule) {
    return rule.pcRelative ? "S + A - P" : "S + A";
}

std::string rangeText(const RelocationRule& rule) {
    switch (rule.range) {
        case RelocationRule::Signed32: return "signed 32-bit";
        case RelocationRule::Unsigned32: return "unsigned 32-bit";
        case RelocationRule::Any64: break;
    }
    return "64-bit";
}

bool fitsField(const RelocationRule& rule, int64_t value) {
    switch (rule.range) {
        case RelocationRule::Signed32: return value >= INT32_MIN && value <= INT32_MAX;
        case RelocationRule::Unsigned32: return value >= 0 && value <= int64_t(UINT32_MAX);
        case RelocationRule::Any64: break;
    }
    return true;
}

bool printAnalysis(const std::vector<ObjectFile>& objects) {
    SymbolTable table = buildSymbolTable(objects);
    printInputSummary(objects);

    std::printf("\n== Symbol resolution: which definition does each name refer to? ==\n\n");
    printDefinitions(objects, table);
    size_t unresolved = printReferences(objects, table);

    printRelocationAnalysis(objects, table);
    return unresolved == 0 && table.errors.empty();
}
