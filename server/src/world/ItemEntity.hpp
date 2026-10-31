#pragma once
#include "inventory/Inventory.hpp"

#include <cstdint>

namespace perlin::server {

    // an item lying in the world: dropped by a player or by a broken block, picked up by walking over it
    struct ItemEntity {
        uint64_t id = 0;
        ItemStack stack;
        double x = 0, y = 0, z = 0;
        double vx = 0, vy = 0, vz = 0;
        int age = 0;
        // ticks before anyone may pick it up
        int pickupDelay = 10;
        bool onGround = false;
    };
}
