//===- CTSOverrides.cpp -- Override SVF weak symbols for CTS frontend ------//
//
// The LLVM frontend normally provides these implementations. Since we use
// TreeSitter instead, we provide simple implementations that return
// the node's name or a generic string.
//
//===----------------------------------------------------------------------===//

#include "SVFIR/SVFValue.h"

namespace SVF
{

/// Override the weak valueOnlyToString to return name + source location
const std::string SVFValue::valueOnlyToString() const
{
    std::string result;
    if (!getName().empty())
        result = " " + getName();
    else
        result = "node_" + std::to_string(getId());
    if (!getSourceLoc().empty())
        result += " " + getSourceLoc();
    return result;
}

/// Override the weak hasLLVMValue to return false (we don't use LLVM)
const bool SVFValue::hasLLVMValue() const
{
    return false;
}

} // namespace SVF
