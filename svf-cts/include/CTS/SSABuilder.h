#ifndef CTS_SSA_BUILDER_H
#define CTS_SSA_BUILDER_H

#include "SVFIR/SVFType.h"
#include <string>
#include <map>
#include <vector>
#include <set>
#include <functional>

namespace SVF
{

class SVFBasicBlock;
class ICFGNode;
class SVFIR;

/// SSA construction using Braun et al. (2013) algorithm.
/// Converts C source (non-SSA) variable assignments into SSA form
/// where each definition creates a new version and phi nodes merge
/// at control-flow join points.
class SSABuilder
{
public:
    /// Pending phi node to be materialized as PhiStmt in SVFIR
    struct PendingPhi
    {
        std::string varName;
        const SVFBasicBlock* block;
        NodeID resultID;
        std::vector<NodeID> operandIDs;
        std::vector<const ICFGNode*> predNodes;
    };

    /// Callback to create a new ValVar node for a phi result
    using CreateNodeFn = std::function<NodeID(const std::string& varName)>;

    SSABuilder() = default;

    /// Set the callback for creating new value nodes
    void setCreateNodeFn(CreateNodeFn fn) { createNode = std::move(fn); }

    /// Record a variable definition in a basic block
    void writeVariable(const std::string& varName, const SVFBasicBlock* block, NodeID valID);

    /// Read a variable's current SSA value in a basic block.
    /// May create phi nodes if needed.
    NodeID readVariable(const std::string& varName, const SVFBasicBlock* block);

    /// Add a predecessor relationship between blocks
    void addBlockPred(const SVFBasicBlock* block, const SVFBasicBlock* pred);

    /// Seal a block (all predecessors are known)
    void sealBlock(const SVFBasicBlock* block);

    /// Get all pending phi nodes to be materialized
    const std::vector<PendingPhi>& getPendingPhis() const { return pendingPhis; }

    /// Clear state for a new function
    void clear();

private:
    NodeID readVariableRecursive(const std::string& varName, const SVFBasicBlock* block);
    NodeID addPhiOperands(const std::string& varName, const SVFBasicBlock* block, NodeID phiNode);
    NodeID tryRemoveTrivialPhi(NodeID phiNode, const std::string& varName, const SVFBasicBlock* block);

    /// Current definition for each variable in each block
    std::map<const SVFBasicBlock*, std::map<std::string, NodeID>> currentDef;

    /// Sealed blocks (all predecessors known)
    std::set<const SVFBasicBlock*> sealedBlocks;

    /// Block predecessors
    std::map<const SVFBasicBlock*, std::vector<const SVFBasicBlock*>> blockPreds;

    /// Incomplete phi nodes for unsealed blocks
    std::map<const SVFBasicBlock*, std::map<std::string, NodeID>> incompletePhis;

    /// All pending phi nodes
    std::vector<PendingPhi> pendingPhis;

    /// Node creation callback
    CreateNodeFn createNode;
};

} // namespace SVF

#endif // CTS_SSA_BUILDER_H
