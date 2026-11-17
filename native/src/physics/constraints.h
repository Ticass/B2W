#pragma once

#include "physics/physics.h"

#include <array>
#include <string>

namespace w2b::physics
{
    inline constexpr std::array JointTypes{"none", "point", "distance", "hinge", "joint", "actuator", "fake_shake", "launch", "rope"};
    inline constexpr int RopeJoint = 8;
    inline constexpr auto ConstraintManifest = "BSP/constraints.json";
    using Vector = std::array<double, 3>;

    std::string JointField(std::string_view field, int endpoint);
    const Json& JointPosition(const Json& joint, int endpoint);
    std::string FormatNumber(double value);
    std::string FormatVector(const Vector& position, std::string_view separator);
}
