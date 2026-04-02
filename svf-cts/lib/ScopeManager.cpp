#include "CTS/ScopeManager.h"

namespace SVF
{

void ScopeManager::pushScope()
{
    scopeStack.emplace_back();
}

void ScopeManager::popScope()
{
    if (!scopeStack.empty())
    {
        scopeStack.pop_back();
    }
}

void ScopeManager::declareVar(const std::string& name, NodeID valNode,
                               NodeID objNode, const SVFType* type)
{
    if (!scopeStack.empty())
    {
        scopeStack.back()[name] = VarInfo{valNode, objNode, type};
    }
}

ScopeManager::VarInfo* ScopeManager::lookupVar(const std::string& name)
{
    // Walk from innermost to outermost scope
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it)
    {
        auto found = it->find(name);
        if (found != it->end())
        {
            return &found->second;
        }
    }
    return nullptr;
}

bool ScopeManager::hasVar(const std::string& name) const
{
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it)
    {
        if (it->find(name) != it->end())
        {
            return true;
        }
    }
    return false;
}

} // namespace SVF
