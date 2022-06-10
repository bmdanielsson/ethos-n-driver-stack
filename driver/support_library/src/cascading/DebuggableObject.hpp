//
// Copyright © 2021-2022 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <string>

#include "Visualisation.hpp"

namespace ethosn
{
namespace support_library
{

struct DebuggableObject
{
public:
    /// Constructor where a debug tag is generated by combining the given prefix with a uniquely
    /// generated ID number.
    DebuggableObject(const char* defaultTagPrefix);

    /// 'Tagged' constructor where the entire debug tag is specified explicitly.
    struct ExplicitDebugTag
    {};
    DebuggableObject(ExplicitDebugTag, const char* debugTag);

    /// This can be used to help identify this object for debugging purposes, and is used in visualisations (dot files)
    /// to identify this object. It shouldn't have any effect on network compilation or estimation.
    std::string m_DebugTag;
    int m_DebugId;

    /// Counter for generating unique debug tags (see DebuggableObject constructor).
    /// This is publicly exposed so can be manipulated by tests.
    static int ms_IdCounter;

    virtual DotAttributes GetDotAttributes(DetailLevel) const
    {
        DotAttributes result;
        result.m_Id    = SanitizeId(m_DebugTag);
        result.m_Label = m_DebugTag;
        return result;
    }
};

}    // namespace support_library
}    // namespace ethosn
