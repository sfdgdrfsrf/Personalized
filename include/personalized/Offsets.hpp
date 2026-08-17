#pragma once

#include <cstddef>
#include <cstdint>

/// Field offsets for MC Bedrock classes (ARM64).
/// Version-specific — update when targeting a new MC version.
/// Based on BedrockTools offsets for ~1.21.70 (protocol 26.20.x).
namespace personalized::offsets {

namespace Actor {
    inline constexpr std::size_t mEntityContext        = 0x8;
    inline constexpr std::size_t mEntityData           = 0x120;
    inline constexpr std::size_t mStateVectorComponent = 0x208;
    inline constexpr std::size_t mActorRotationComponent = 0x218;
    inline constexpr std::size_t mLevel                = 464;
    inline constexpr std::size_t mDimension            = 448;
    inline constexpr std::size_t mHurtTime             = 0x194;
    inline constexpr std::size_t mCategories            = 512;
    inline constexpr std::size_t mNameTagHash           = 384;
    inline constexpr std::size_t mFilteredNameTag       = 712;
}

namespace Player {
    inline constexpr std::size_t mName = 2824;
    inline constexpr std::size_t mSkin = 2552;
}

namespace Level {
    inline constexpr std::size_t mActorManager      = 0x470;
    inline constexpr std::size_t mHitResultWrapper  = 456;
}

} // namespace personalized::offsets
