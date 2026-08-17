#pragma once
#include <string>

class Player {
public:
    virtual ~Player() = default;
    std::string getRealName() const { return "stub"; }
    struct Uuid { std::string asString() const { return "00000000-0000-0000-0000-000000000000"; } };
    Uuid getUuid() const { return {}; }
    bool isPlayer() const { return true; }
    void kill() {}
    void refreshInventory() {}

    struct ItemStub {};
    struct InventoryStub {
        int getContainerSize() const { return 0; }
        ItemStub* getItem(int) { static ItemStub s; return &s; }
        void setItem(int, const ItemStub&) {}
    };
    InventoryStub& getInventory() { static InventoryStub s; return s; }
    void add(const ItemStub&) {}
};
