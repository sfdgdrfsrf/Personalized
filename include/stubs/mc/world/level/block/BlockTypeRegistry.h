#pragma once

namespace mc {

class BlockTypeRegistry {
public:
    static BlockLegacy* lookupByName(const std::string&) { return nullptr; }
};

} // namespace mc
