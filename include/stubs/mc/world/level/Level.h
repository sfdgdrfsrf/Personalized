#pragma once

namespace mc {

class Level {
public:
    static Level& get() {
        static Level inst;
        return inst;
    }
};

} // namespace mc
