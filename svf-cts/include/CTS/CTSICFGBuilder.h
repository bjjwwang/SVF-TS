#ifndef CTS_ICFG_BUILDER_H
#define CTS_ICFG_BUILDER_H

#include "CTS/CTSModule.h"
#include "Graphs/ICFG.h"
#include "SVFIR/SVFVariables.h"

#include <tree_sitter/api.h>
#include <map>

namespace SVF
{

/// Builds ICFG from TreeSitter AST.
/// Extends ICFG to access protected factory methods.
class CTSICFGBuilder : public ICFG
{
public:
    CTSICFGBuilder();
    ~CTSICFGBuilder() override;

    /// Build ICFG from CTSModuleSet.
    /// FunObjVar nodes must already be created for all functions.
    /// Returns 'this' (the builder IS the ICFG).
    ICFG* build(CTSModuleSet* moduleSet);

    /// Get the ICFG node created for a statement
    ICFGNode* getStmtICFGNode(TSNode node, CTSSourceFile* file) const;

    /// Get basic block for a node
    const SVFBasicBlock* getStmtBB(TSNode node, CTSSourceFile* file) const;

private:
    void buildFunctionICFG(CTSFunction* func);

    /// Process statements and return first/last ICFG nodes
    struct ICFGNodePair { ICFGNode* first; ICFGNode* last; };

    ICFGNodePair processCompoundStmt(TSNode compound, CTSSourceFile* file,
                                     const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processStatement(TSNode stmt, CTSSourceFile* file,
                                  const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processIfStmt(TSNode ifStmt, CTSSourceFile* file,
                               const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processWhileStmt(TSNode whileStmt, CTSSourceFile* file,
                                  const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processForStmt(TSNode forStmt, CTSSourceFile* file,
                                const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processSwitchStmt(TSNode switchStmt, CTSSourceFile* file,
                                   const FunObjVar* func, const SVFBasicBlock* bb);
    ICFGNodePair processDoWhileStmt(TSNode doStmt, CTSSourceFile* file,
                                    const FunObjVar* func, const SVFBasicBlock* bb);

    bool isReturnStmt(TSNode node) const;
    bool containsCall(TSNode node, CTSSourceFile* file) const;

    /// Find a direct call_expression inside a statement node.
    /// Returns the call_expression TSNode if found, else a null TSNode.
    TSNode findDirectCallExpr(TSNode stmt) const;

    /// Find ALL call_expression nodes in a subtree (bottom-up: inner calls first).
    void findAllCallExprs(TSNode node, std::vector<TSNode>& results) const;

    /// Look up the FunObjVar for a callee identifier
    const FunObjVar* lookupCallee(TSNode funcNode, CTSSourceFile* file) const;

    /// Track statement → ICFG node mapping
    void recordStmtNode(TSNode node, CTSSourceFile* file, ICFGNode* icfgNode);

public:
    /// Set branch condition on an IntraCFGEdge (needs friend access via ICFG)
    void setEdgeCondition(IntraCFGEdge* edge, const SVFVar* condVar, s64_t branchVal);

private:
    CTSModuleSet* moduleSet;
    std::map<std::pair<CTSSourceFile*, uint32_t>, ICFGNode*> stmtToICFGNode;
    std::map<std::pair<CTSSourceFile*, uint32_t>, const SVFBasicBlock*> stmtToBB;
};

} // namespace SVF

#endif // CTS_ICFG_BUILDER_H
