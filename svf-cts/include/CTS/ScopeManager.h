#ifndef CTS_SCOPE_MANAGER_H
#define CTS_SCOPE_MANAGER_H

#include "SVFIR/SVFType.h"
#include <string>
#include <map>
#include <vector>

namespace SVF
{

/// Manages variable scoping and name resolution for C source
class ScopeManager
{
public:
    struct VarInfo
    {
        NodeID valNode;       // The value node (pointer to the variable)
        NodeID objNode;       // The object node (the variable's memory)
        const SVFType* type;  // The variable's type
    };

    ScopeManager() = default;

    /// Push a new scope (entering function body, block, loop body, etc.)
    void pushScope();

    /// Pop the current scope
    void popScope();

    /// Declare a variable in the current scope
    void declareVar(const std::string& name, NodeID valNode, NodeID objNode, const SVFType* type);

    /// Look up a variable by name (walks scope chain from innermost to outermost)
    /// Returns nullptr if not found
    VarInfo* lookupVar(const std::string& name);

    /// Check if a variable exists in any scope
    bool hasVar(const std::string& name) const;

    /// Get current scope depth
    size_t depth() const { return scopeStack.size(); }

private:
    std::vector<std::map<std::string, VarInfo>> scopeStack;
};

} // namespace SVF

#endif // CTS_SCOPE_MANAGER_H
