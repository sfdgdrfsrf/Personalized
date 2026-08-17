#pragma once

namespace mc {

class ClientInstance {
public:
    static ClientInstance& get() {
        static ClientInstance inst;
        return inst;
    }
};

} // namespace mc
