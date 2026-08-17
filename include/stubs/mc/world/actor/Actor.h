#pragma once
#include <string>

namespace mc {

class Actor {
public:
    virtual ~Actor() = default;
    bool isPlayer() const { return false; }
    std::string getTypeName() const { return "minecraft:actor"; }
};

} // namespace mc
