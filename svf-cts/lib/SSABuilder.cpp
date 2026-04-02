#include "CTS/SSABuilder.h"

namespace SVF
{

void SSABuilder::writeVariable(const std::string& varName,
                                const SVFBasicBlock* block, NodeID valID)
{
    currentDef[block][varName] = valID;
}

NodeID SSABuilder::readVariable(const std::string& varName,
                                 const SVFBasicBlock* block)
{
    auto blockIt = currentDef.find(block);
    if (blockIt != currentDef.end())
    {
        auto varIt = blockIt->second.find(varName);
        if (varIt != blockIt->second.end())
        {
            return varIt->second;
        }
    }
    // Variable not defined in this block, search predecessors
    return readVariableRecursive(varName, block);
}

NodeID SSABuilder::readVariableRecursive(const std::string& varName,
                                          const SVFBasicBlock* block)
{
    NodeID val;

    auto predIt = blockPreds.find(block);
    bool hasNoPreds = (predIt == blockPreds.end() || predIt->second.empty());

    if (!sealedBlocks.count(block))
    {
        // Block not sealed yet: create an incomplete phi
        if (createNode)
        {
            val = createNode(varName);
        }
        else
        {
            val = 0; // Should not happen
        }
        incompletePhis[block][varName] = val;
    }
    else if (hasNoPreds)
    {
        // Entry block or unreachable: no definition found
        // Return a dummy/blackhole node
        val = 0;
    }
    else if (predIt->second.size() == 1)
    {
        // Only one predecessor: no phi needed
        val = readVariable(varName, predIt->second[0]);
    }
    else
    {
        // Multiple predecessors: need a phi node
        if (createNode)
        {
            val = createNode(varName);
        }
        else
        {
            val = 0;
        }
        writeVariable(varName, block, val);
        val = addPhiOperands(varName, block, val);
    }

    writeVariable(varName, block, val);
    return val;
}

NodeID SSABuilder::addPhiOperands(const std::string& varName,
                                   const SVFBasicBlock* block, NodeID phiNode)
{
    PendingPhi phi;
    phi.varName = varName;
    phi.block = block;
    phi.resultID = phiNode;

    auto predIt = blockPreds.find(block);
    if (predIt != blockPreds.end())
    {
        for (const SVFBasicBlock* pred : predIt->second)
        {
            NodeID opnd = readVariable(varName, pred);
            phi.operandIDs.push_back(opnd);
            // predNodes will be filled by the SVFIR builder when materializing
        }
    }

    pendingPhis.push_back(std::move(phi));
    return tryRemoveTrivialPhi(phiNode, varName, block);
}

NodeID SSABuilder::tryRemoveTrivialPhi(NodeID phiNode, const std::string& varName,
                                        const SVFBasicBlock* block)
{
    // Check if all operands are the same (or the phi itself)
    NodeID same = (NodeID)-1;

    // Find the pending phi we just created
    for (auto& phi : pendingPhis)
    {
        if (phi.resultID == phiNode && phi.varName == varName)
        {
            for (NodeID op : phi.operandIDs)
            {
                if (op == phiNode || op == same)
                {
                    continue; // Skip self-references and already-seen value
                }
                if (same != (NodeID)-1)
                {
                    return phiNode; // Non-trivial phi: different operands
                }
                same = op;
            }
            break;
        }
    }

    if (same == (NodeID)-1)
    {
        return phiNode; // Unreachable or self-referential; keep it
    }

    // Trivial phi: replace with the single value
    // Remove the pending phi
    for (auto it = pendingPhis.begin(); it != pendingPhis.end(); ++it)
    {
        if (it->resultID == phiNode && it->varName == varName)
        {
            pendingPhis.erase(it);
            break;
        }
    }

    // Update the definition
    writeVariable(varName, block, same);
    return same;
}

void SSABuilder::addBlockPred(const SVFBasicBlock* block, const SVFBasicBlock* pred)
{
    blockPreds[block].push_back(pred);
}

void SSABuilder::sealBlock(const SVFBasicBlock* block)
{
    sealedBlocks.insert(block);

    // Fill in incomplete phis
    auto incIt = incompletePhis.find(block);
    if (incIt != incompletePhis.end())
    {
        for (auto& pair : incIt->second)
        {
            addPhiOperands(pair.first, block, pair.second);
        }
        incompletePhis.erase(incIt);
    }
}

void SSABuilder::clear()
{
    currentDef.clear();
    sealedBlocks.clear();
    blockPreds.clear();
    incompletePhis.clear();
    pendingPhis.clear();
}

} // namespace SVF
