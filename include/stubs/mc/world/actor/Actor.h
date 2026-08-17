#pragma once
#include <string>

class Actor {
public:
    virtual ~Actor() = default;
    bool isPlayer() const { return false; }
    std::string getTypeName() const { return "minecraft:zombie"; }
    struct Vec3 { float x=0,y=0,z=0; };
    Vec3 getPosition() const { return {}; }

    struct DimensionStub {
        void* spawnEntity(const std::string&, Vec3) { return nullptr; }
    };
    DimensionStub* getDimension() { static DimensionStub s; return &s; }
    void remove() {}
};
