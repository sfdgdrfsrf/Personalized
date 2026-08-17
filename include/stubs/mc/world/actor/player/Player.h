#pragma once
#include <string>

namespace mc {

/// Minimal Player stub for event references
class Player {
public:
    std::string getName() const { return "StubPlayer"; }
};

} // namespace mc
