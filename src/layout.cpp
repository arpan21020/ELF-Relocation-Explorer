#include "layout.h"

#include "relocation_analyzer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <set>

namespace {

const char* const kOutputOrder[] = {".text", ".rodata", ".data", ".bss"};

std::string outputSectionFor(const Section& section) {
    if ((section.flags & SHF_ALLOC) == 0) {
        return "";
    }
    for (const char* name : kOutputOrder) {
        std::string prefix = name;
        if (section.name == prefix || section.name.rfind(prefix + ".", 0) == 0) {
            return prefix;
        }
    }
    return "";
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct Patcher {
    const std::vector<ObjectFile>& objects;
    const SymbolTable& table;
    const Layout& layout;
    std::map<SectionKey, std::vector<uint8_t>> contents;
    std::set<SectionKey> patched;
    int applied = 0;
    int skipped = 0;
    int failed = 0;
};

std::string location(const std::vector<ObjectFile>& objects, size_t file, size_t section) {
    const ObjectFile& obj = objects[file];
    return fileName(obj.path) + ":" + obj.sections[section].name;
}

std::string fieldHex(int64_t value, unsigned size) {
    char buffer[24];
    if (size == 8) {
        std::snprintf(buffer, sizeof(buffer), "0x%016" PRIx64, static_cast<uint64_t>(value));
    } else {
        std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32, static_cast<uint32_t>(value));
    }
    return buffer;
}

std::string byteList(const std::vector<uint8_t>& bytes, uint64_t begin, uint64_t end) {
    std::string text;
    for (uint64_t i = begin; i < end; ++i) {
        char buffer[4];
        std::snprintf(buffer, sizeof(buffer), "%02x", bytes[i]);
        text += (text.empty() ? "" : " ") + std::string(buffer);
    }
    return text;
}

std::string bytesInContext(const std::vector<uint8_t>& bytes, uint64_t offset, unsigned size) {
    uint64_t begin = offset >= 4 ? offset - 4 : 0;
    uint64_t end = std::min<uint64_t>(bytes.size(), offset + size + 2);
    std::string before = byteList(bytes, begin, offset);
    std::string after = byteList(bytes, offset + size, end);
    return (before.empty() ? "" : before + " ") + "[" + byteList(bytes, offset, offset + size) + "]" +
           (after.empty() ? "" : " " + after);
}

std::string symbolAt(const ObjectFile& obj, uint16_t section, uint64_t offset) {
    for (const Symbol& symbol : obj.symbols) {
        bool named = symbol.type == STT_FUNC || symbol.type == STT_OBJECT;
        uint64_t size = std::max<uint64_t>(symbol.size, 1);
        if (!named || symbol.sectionIndex != section || offset < symbol.value || offset - symbol.value >= size) {
            continue;
        }
        uint64_t delta = offset - symbol.value;
        return "<" + symbol.name + (delta == 0 ? "" : "+" + hex(delta)) + ">";
    }
    return "";
}

void fail(Patcher& p, const std::string& message) {
    std::printf("    error     %s\n", message.c_str());
    ++p.failed;
}

bool symbolAddress(const Patcher& p, const SymbolTarget& target, uint64_t& address, std::string& how,
                   std::string& why) {
    switch (target.kind) {
        case SymbolTarget::InSection: {
            auto found = p.layout.addressOf.find({target.file, target.section});
            std::string where = location(p.objects, target.file, target.section);
            if (found == p.layout.addressOf.end()) {
                why = "section " + where + " of '" + target.name + "' is not part of the assumed layout";
                return false;
            }
            address = found->second + target.value;
            how = where + " (" + hex(found->second) + ") + " + hex(target.value);
            return true;
        }
        case SymbolTarget::Absolute:
            address = target.value;
            how = "absolute";
            return true;
        case SymbolTarget::WeakUndefined:
            address = 0;
            how = "undefined weak symbol";
            return true;
        case SymbolTarget::Common:
            why = "'" + target.name + "' is a COMMON symbol, which this tool does not allocate";
            return false;
        case SymbolTarget::Undefined:
            break;
    }
    why = "undefined symbol '" + target.name + "'";
    return false;
}

void writeLittleEndian(std::vector<uint8_t>& bytes, uint64_t offset, int64_t value, unsigned size) {
    uint64_t bits = static_cast<uint64_t>(value);
    for (unsigned i = 0; i < size; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(bits >> (8 * i));
    }
}

void printCheck(const Patcher& p, const SymbolTarget& target, uint64_t P, int64_t D, int64_t A) {
    uint64_t reached = P + 4 + static_cast<uint64_t>(D);
    std::string text = "P + 4 + D = " + hex(reached);
    if (target.kind == SymbolTarget::InSection) {
        uint64_t offset = target.value + static_cast<uint64_t>(A + 4);
        const ObjectFile& obj = p.objects[target.file];
        text += " -> " + location(p.objects, target.file, target.section) + " + " + hex(offset);
        std::string name = symbolAt(obj, target.section, offset);
        if (!name.empty()) {
            text += " " + name;
        }
    }
    std::printf("    check     %s\n", text.c_str());
}

void applyRelocation(Patcher& p, size_t file, const RelocationSection& rela, const Relocation& reloc) {
    const ObjectFile& obj = p.objects[file];
    SectionKey key{file, rela.targetSection};
    std::printf("\n%s + %s   %s   %s %s\n", location(p.objects, file, rela.targetSection).c_str(),
                hex(reloc.offset).c_str(), relocationTypeName(reloc.type).c_str(),
                symbolDisplayName(obj, reloc.symbolIndex).c_str(), addendText(reloc.addend).c_str());

    auto base = p.layout.addressOf.find(key);
    if (base == p.layout.addressOf.end()) {
        std::printf("    skipped   target section %s is not part of the assumed layout\n",
                    obj.sections[rela.targetSection].name.c_str());
        ++p.skipped;
        return;
    }
    const RelocationRule* rule = findRelocationRule(reloc.type);
    if (rule == nullptr) {
        fail(p, "unsupported relocation type");
        return;
    }
    std::vector<uint8_t>& bytes = p.contents[key];
    if (reloc.offset > bytes.size() || bytes.size() - reloc.offset < rule->fieldSize) {
        fail(p, "relocation field lies outside the section contents");
        return;
    }

    SymbolTarget target = resolveSymbol(p.objects, p.table, file, reloc.symbolIndex);
    uint64_t S = 0;
    std::string how;
    std::string why;
    if (!symbolAddress(p, target, S, how, why)) {
        fail(p, "cannot compute: " + why);
        return;
    }
    uint64_t P = base->second + reloc.offset;
    int64_t A = reloc.addend;
    int64_t D = static_cast<int64_t>(S) + A - (rule->pcRelative ? static_cast<int64_t>(P) : 0);

    std::printf("    location  P = %s (%s) + %s = %s\n", location(p.objects, file, rela.targetSection).c_str(),
                hex(base->second).c_str(), hex(reloc.offset).c_str(), hex(P).c_str());
    std::printf("    bytes     %s\n", bytesInContext(bytes, reloc.offset, rule->fieldSize).c_str());
    std::printf("    symbol    S = %s = %s  (%s)\n", how.c_str(), hex(S).c_str(), target.name.c_str());
    if (rule->pcRelative) {
        std::printf("    formula   D = S + A - P = %s + (%" PRId64 ") - %s\n", hex(S).c_str(), A, hex(P).c_str());
    } else {
        std::printf("    formula   D = S + A = %s + (%" PRId64 ")\n", hex(S).c_str(), A);
    }
    std::printf("    result    D = %s (%" PRId64 ")\n", fieldHex(D, rule->fieldSize).c_str(), D);
    if (!fitsField(*rule, D)) {
        fail(p, "overflow: D does not fit in a " + rangeText(*rule) + " field");
        return;
    }

    std::string before = byteList(bytes, reloc.offset, reloc.offset + rule->fieldSize);
    writeLittleEndian(bytes, reloc.offset, D, rule->fieldSize);
    std::string after = byteList(bytes, reloc.offset, reloc.offset + rule->fieldSize);
    std::printf("    patch     %s -> %s\n", before.c_str(), after.c_str());
    if (rule->pcRelative) {
        printCheck(p, target, P, D, A);
    }
    p.patched.insert(key);
    ++p.applied;
}

void printLayout(const std::vector<ObjectFile>& objects, const Layout& layout) {
    std::printf("== ASSUMED LAYOUT — a real linker may place sections differently ==\n\n");
    std::printf("  %-8s %-16s %-16s %8s %6s  %s\n", "Output", "Input file", "Input section", "Size", "Align",
                "Address");
    for (const OutputSection& out : layout.sections) {
        for (const InputPlacement& in : out.inputs) {
            const ObjectFile& obj = objects[in.section.first];
            const Section& section = obj.sections[in.section.second];
            std::printf("  %-8s %-16s %-16s %8s %6s  %s\n", out.name.c_str(), fileName(obj.path).c_str(),
                        section.name.c_str(), hex(section.size).c_str(), hex(section.addralign).c_str(),
                        hex(in.address).c_str());
        }
    }
}

void printPatchedContents(const Patcher& p) {
    std::printf("\n== Patched section contents (in memory only) ==\n");
    for (const SectionKey& key : p.patched) {
        const std::vector<uint8_t>& bytes = p.contents.at(key);
        uint64_t address = p.layout.addressOf.at(key);
        std::printf("\n%s at %s:\n", location(p.objects, key.first, key.second).c_str(), hex(address).c_str());
        for (uint64_t row = 0; row < bytes.size(); row += 16) {
            uint64_t end = std::min<uint64_t>(bytes.size(), row + 16);
            std::printf("  %s  %s\n", hex(address + row).c_str(), byteList(bytes, row, end).c_str());
        }
    }
}

}

Layout buildLayout(const std::vector<ObjectFile>& objects, uint64_t base) {
    Layout layout;
    uint64_t cursor = base;
    for (const char* name : kOutputOrder) {
        OutputSection out;
        out.name = name;
        for (size_t file = 0; file < objects.size(); ++file) {
            const std::vector<Section>& sections = objects[file].sections;
            for (size_t i = 0; i < sections.size(); ++i) {
                if (outputSectionFor(sections[i]) != out.name) {
                    continue;
                }
                cursor = alignUp(cursor, std::max<uint64_t>(sections[i].addralign, 1));
                if (out.inputs.empty()) {
                    out.address = cursor;
                }
                out.inputs.push_back({{file, i}, cursor});
                layout.addressOf[{file, i}] = cursor;
                cursor += sections[i].size;
            }
        }
        if (!out.inputs.empty()) {
            out.size = cursor - out.address;
            layout.sections.push_back(out);
        }
    }
    return layout;
}

bool printRelocation(const std::vector<ObjectFile>& objects, uint64_t base) {
    SymbolTable table = buildSymbolTable(objects);
    Layout layout = buildLayout(objects, base);
    printLayout(objects, layout);

    for (const std::string& error : table.errors) {
        std::printf("\nerror: %s (using the first definition)\n", error.c_str());
    }

    Patcher p{objects, table, layout, {}, {}, 0, 0, 0};
    for (const auto& [key, address] : layout.addressOf) {
        (void)address;
        p.contents[key] = sectionContents(objects[key.first], key.second);
    }

    std::printf("\n== Relocations ==\n");
    for (size_t file = 0; file < objects.size(); ++file) {
        for (const RelocationSection& rela : objects[file].relocations) {
            for (const Relocation& reloc : rela.entries) {
                applyRelocation(p, file, rela, reloc);
            }
        }
    }

    printPatchedContents(p);
    std::printf("\nSummary: %d applied, %d skipped, %d failed\n", p.applied, p.skipped, p.failed);
    return p.failed == 0 && table.errors.empty();
}
