#pragma once

#include "elf_parser.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

using SectionKey = std::pair<size_t, size_t>;

struct InputPlacement {
    SectionKey section;
    uint64_t address = 0;
};

struct OutputSection {
    std::string name;
    uint64_t address = 0;
    uint64_t size = 0;
    std::vector<InputPlacement> inputs;
};

struct Layout {
    std::vector<OutputSection> sections;
    std::map<SectionKey, uint64_t> addressOf;
};

Layout buildLayout(const std::vector<ObjectFile>& objects, uint64_t base);

bool printRelocation(const std::vector<ObjectFile>& objects, uint64_t base);
