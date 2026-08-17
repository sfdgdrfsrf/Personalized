#pragma once
#include <string>

namespace mc {

class ItemInstance {};

class Item {
public:
    std::string getDescriptionId() const { return "item.minecraft.unknown"; }
};

class CreativeItemRegistry {
public:
    static std::vector<ItemInstance>& getCreativeItems() {
        static std::vector<ItemInstance> items;
        return items;
    }
};

class ItemRegistry {
public:
    static Item* getItem(const std::string&) { return nullptr; }
};

} // namespace mc
