#pragma once

namespace mc {

class BlockPalette {
public:
    static BlockPalette& get() {
        static BlockPalette inst;
        return inst;
    }
};

} // namespace mc
