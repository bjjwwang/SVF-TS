#include "CTS/CTSICFGBuilder.h"
#include "CTS/CTSParser.h"
#include "Graphs/ICFGNode.h"
#include "Graphs/ICFGEdge.h"
#include "Util/SVFUtil.h"

#include <cstring>

namespace SVF
{

CTSICFGBuilder::CTSICFGBuilder()
    : ICFG(), moduleSet(nullptr)
{
}

CTSICFGBuilder::~CTSICFGBuilder()
{
}

ICFG* CTSICFGBuilder::build(CTSModuleSet* modSet)
{
    moduleSet = modSet;

    // Create GlobalICFGNode (required by AbstractInterpretation)
    GlobalICFGNode* globalNode = new GlobalICFGNode(totalICFGNode++);
    addGlobalICFGNode(globalNode);

    // Create entry/exit nodes for declaration-only functions FIRST
    // (needed before building body ICFGs, since calls may reference them)
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;
        if (!func->hasBody() && func->getFunObjVar())
        {
            const FunObjVar* svfFunc = func->getFunObjVar();
            FunEntryICFGNode* entry = addFunEntryICFGNode(svfFunc);
            FunExitICFGNode* exit = addFunExitICFGNode(svfFunc);

            // Register in basic blocks
            if (svfFunc->begin() != svfFunc->end())
            {
                const SVFBasicBlock* entryBB = svfFunc->getEntryBlock();
                const SVFBasicBlock* exitBB = svfFunc->getExitBB();
                const_cast<SVFBasicBlock*>(entryBB)->addICFGNode(entry);
                const_cast<SVFBasicBlock*>(exitBB)->addICFGNode(exit);
            }

            addIntraEdge(entry, exit);
        }
    }

    // Pre-create entry/exit nodes for ALL functions with bodies
    // (needed because body processing may reference other functions' entry nodes)
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;
        if (func->hasBody() && func->getFunObjVar())
        {
            const FunObjVar* svfFunc = func->getFunObjVar();
            FunEntryICFGNode* entry = addFunEntryICFGNode(svfFunc);
            FunExitICFGNode* exit = addFunExitICFGNode(svfFunc);

            if (svfFunc->begin() != svfFunc->end())
            {
                const SVFBasicBlock* entryBB = svfFunc->getEntryBlock();
                const SVFBasicBlock* exitBB = svfFunc->getExitBB();
                const_cast<SVFBasicBlock*>(entryBB)->addICFGNode(entry);
                const_cast<SVFBasicBlock*>(exitBB)->addICFGNode(exit);
            }
        }
    }

    // Build ICFG bodies for each function (entry/exit already exist)
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;
        if (func->hasBody() && func->getFunObjVar())
        {
            buildFunctionICFG(func);
        }
    }

    // Add edge from GlobalICFGNode to main's entry (if main exists)
    CTSFunction* mainFunc = moduleSet->getFunction("main");
    if (mainFunc && mainFunc->getFunObjVar())
    {
        FunEntryICFGNode* mainEntry = getFunEntryICFGNode(mainFunc->getFunObjVar());
        if (mainEntry)
        {
            IntraCFGEdge* edge = new IntraCFGEdge(getGlobalICFGNode(), mainEntry);
            edge->getDstNode()->addIncomingEdge(edge);
            edge->getSrcNode()->addOutgoingEdge(edge);
        }
    }

    return this;
}

void CTSICFGBuilder::buildFunctionICFG(CTSFunction* tsFunc)
{
    const FunObjVar* svfFunc = tsFunc->getFunObjVar();
    if (!svfFunc) return;

    // Entry/exit nodes are already created in the pre-pass
    FunEntryICFGNode* entryNode = getFunEntryICFGNode(svfFunc);
    FunExitICFGNode* exitNode = getFunExitICFGNode(svfFunc);

    const SVFBasicBlock* entryBB = nullptr;
    if (svfFunc->begin() != svfFunc->end())
    {
        entryBB = svfFunc->getEntryBlock();
    }

    TSNode body = tsFunc->getBody();
    CTSSourceFile* file = tsFunc->getSourceFile();

    if (!ts_node_is_null(body))
    {

        auto result = processCompoundStmt(body, file, svfFunc, entryBB);

        if (result.first)
        {
            addIntraEdge(entryNode, result.first);
        }
        // The last node connects to exit if no explicit return was processed
        if (result.last && result.last != exitNode)
        {
            // Check if there's already an edge to exit (from return statements)
            bool hasExitEdge = false;
            for (auto it = result.last->OutEdgeBegin(); it != result.last->OutEdgeEnd(); ++it)
            {
                if ((*it)->getDstNode() == exitNode) { hasExitEdge = true; break; }
            }
            if (!hasExitEdge)
            {
                addIntraEdge(result.last, exitNode);
            }
        }
        if (!result.first)
        {
            addIntraEdge(entryNode, exitNode);
        }
    }
    else
    {
        addIntraEdge(entryNode, exitNode);
    }
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processCompoundStmt(
    TSNode compound, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    ICFGNodePair result = {nullptr, nullptr};
    if (ts_node_is_null(compound)) return result;

    uint32_t childCount = ts_node_named_child_count(compound);
    for (uint32_t i = 0; i < childCount; i++)
    {
        TSNode child = ts_node_named_child(compound, i);
        auto stmtResult = processStatement(child, file, func, bb);

        if (stmtResult.first)
        {
            if (!result.first) result.first = stmtResult.first;
            if (result.last && result.last != stmtResult.first)
            {
                addIntraEdge(result.last, stmtResult.first);
            }
            result.last = stmtResult.last;
        }
    }

    return result;
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processStatement(
    TSNode stmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    ICFGNodePair result = {nullptr, nullptr};
    if (ts_node_is_null(stmt) || !bb) return result;

    const char* type = ts_node_type(stmt);

    if (strcmp(type, "compound_statement") == 0)
    {
        return processCompoundStmt(stmt, file, func, bb);
    }

    if (strcmp(type, "if_statement") == 0)
    {
        return processIfStmt(stmt, file, func, bb);
    }

    if (strcmp(type, "while_statement") == 0)
    {
        return processWhileStmt(stmt, file, func, bb);
    }

    if (strcmp(type, "for_statement") == 0)
    {
        return processForStmt(stmt, file, func, bb);
    }

    if (strcmp(type, "switch_statement") == 0)
    {
        return processSwitchStmt(stmt, file, func, bb);
    }

    if (strcmp(type, "do_statement") == 0)
    {
        return processDoWhileStmt(stmt, file, func, bb);
    }

    // Find ALL call_expressions in this statement (bottom-up: inner calls first)
    std::vector<TSNode> allCalls;
    findAllCallExprs(stmt, allCalls);

    if (!allCalls.empty())
    {
        ICFGNode* firstNode = nullptr;
        ICFGNode* lastNode = nullptr;
        bool anyResolved = false;

        for (const TSNode& callExpr : allCalls)
        {
            TSNode calleeFuncNode = ts_node_child_by_field_name(callExpr, "function", 8);
            const FunObjVar* calledFunc = lookupCallee(calleeFuncNode, file);

            if (calledFunc)
            {
                // Create CallICFGNode for this specific call
                CallICFGNode* callNode = addCallICFGNode(
                    bb, moduleSet->getPtrType(), calledFunc,
                    false /*isVararg*/, false /*isVcall*/, 0 /*vcallIdx*/, "" /*vcallName*/);
                const_cast<SVFBasicBlock*>(bb)->addICFGNode(callNode);
                callNode->setSourceLoc(CTSParser::formatSourceLoc(callExpr, file->getFilePath()));

                // Record by call_expression's own byte offset (unique per call)
                recordStmtNode(callExpr, file, callNode);

                // Create RetICFGNode
                RetICFGNode* retNode = addRetICFGNode(callNode);
                const_cast<SVFBasicBlock*>(bb)->addICFGNode(retNode);

                if (SVFUtil::isExtCall(calledFunc))
                {
                    // External functions: direct intra edge from call to ret
                    // (matching LLVM frontend behavior — AE needs this to
                    //  propagate state through external call sites)
                    addIntraEdge(callNode, retNode);
                }
                else
                {
                    // Internal functions: interprocedural call/ret edges
                    FunEntryICFGNode* calleeEntry = getFunEntryICFGNode(calledFunc);
                    if (calleeEntry)
                        addCallEdge(callNode, calleeEntry);

                    FunExitICFGNode* calleeExit = getFunExitICFGNode(calledFunc);
                    if (calleeExit)
                        addRetEdge(calleeExit, retNode);
                }

                // Chain: previous retNode → this callNode
                if (lastNode)
                    addIntraEdge(lastNode, callNode);

                if (!firstNode) firstNode = callNode;
                lastNode = retNode;
                anyResolved = true;
            }
        }

        if (anyResolved)
        {
            // Record statement-level mapping to the first call node
            recordStmtNode(stmt, file, firstNode);

            bool isRet = isReturnStmt(stmt);
            if (isRet && lastNode)
            {
                FunExitICFGNode* exitNode = getFunExitICFGNode(func);
                if (exitNode) addIntraEdge(lastNode, exitNode);
            }

            return {firstNode, lastNode};
        }
        // All calls had unknown callees → fall through to IntraICFGNode
    }

    // Default: create an IntraICFGNode
    bool isRet = isReturnStmt(stmt);
    IntraICFGNode* node = addIntraICFGNode(bb, isRet);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(node);
    node->setSourceLoc(CTSParser::formatSourceLoc(stmt, file->getFilePath()));
    recordStmtNode(stmt, file, node);

    if (isRet)
    {
        FunExitICFGNode* exitNode = getFunExitICFGNode(func);
        if (exitNode) addIntraEdge(node, exitNode);
    }

    return {node, node};
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processIfStmt(
    TSNode ifStmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    ICFGNode* firstNode = nullptr;  // first node in the chain (call or condNode)

    // Step 1: Process any function calls inside the condition BEFORE the condNode.
    // e.g., if(nd()) or if(foo() > 5) — the call needs its own CallICFGNode.
    TSNode condExpr = ts_node_child_by_field_name(ifStmt, "condition", 9);
    if (!ts_node_is_null(condExpr) &&
        strcmp(ts_node_type(condExpr), "parenthesized_expression") == 0)
        condExpr = ts_node_named_child(condExpr, 0);

    ICFGNode* condCallLast = nullptr;  // last node from condition's call chain
    if (!ts_node_is_null(condExpr))
    {
        std::vector<TSNode> condCalls;
        findAllCallExprs(condExpr, condCalls);
        if (!condCalls.empty())
        {
            // Build call chain for calls in condition (same logic as processStatement)
            ICFGNode* chainFirst = nullptr;
            ICFGNode* chainLast = nullptr;
            for (const TSNode& callExpr : condCalls)
            {
                TSNode calleeFuncNode = ts_node_child_by_field_name(callExpr, "function", 8);
                const FunObjVar* calledFunc = lookupCallee(calleeFuncNode, file);
                if (!calledFunc) continue;

                CallICFGNode* callNode = addCallICFGNode(
                    bb, moduleSet->getPtrType(), calledFunc,
                    false, false, 0, "");
                const_cast<SVFBasicBlock*>(bb)->addICFGNode(callNode);
                callNode->setSourceLoc(CTSParser::formatSourceLoc(callExpr, file->getFilePath()));
                recordStmtNode(callExpr, file, callNode);

                RetICFGNode* retNode = addRetICFGNode(callNode);
                const_cast<SVFBasicBlock*>(bb)->addICFGNode(retNode);

                if (SVFUtil::isExtCall(calledFunc))
                    addIntraEdge(callNode, retNode);
                else
                {
                    FunEntryICFGNode* calleeEntry = getFunEntryICFGNode(calledFunc);
                    if (calleeEntry) addCallEdge(callNode, calleeEntry);
                    FunExitICFGNode* calleeExit = getFunExitICFGNode(calledFunc);
                    if (calleeExit) addRetEdge(calleeExit, retNode);
                }

                if (chainLast) addIntraEdge(chainLast, callNode);
                if (!chainFirst) chainFirst = callNode;
                chainLast = retNode;
            }
            firstNode = chainFirst;
            condCallLast = chainLast;
        }
    }

    // Step 2: Condition node (for branch decision)
    IntraICFGNode* condNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(condNode);
    condNode->setSourceLoc(CTSParser::formatSourceLoc(ifStmt, file->getFilePath()));
    recordStmtNode(ifStmt, file, condNode);

    // Chain condition calls → condNode
    if (condCallLast)
        addIntraEdge(condCallLast, condNode);
    if (!firstNode)
        firstNode = condNode;

    // Merge node (after if/else)
    IntraICFGNode* mergeNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(mergeNode);

    // Process then branch
    TSNode consequence = ts_node_child_by_field_name(ifStmt, "consequence", 11);
    if (!ts_node_is_null(consequence))
    {
        auto thenResult = processStatement(consequence, file, func, bb);
        if (thenResult.first)
        {
            addIntraEdge(condNode, thenResult.first);
            if (thenResult.last) addIntraEdge(thenResult.last, mergeNode);
        }
        else
        {
            addIntraEdge(condNode, mergeNode);
        }
    }
    else
    {
        addIntraEdge(condNode, mergeNode);
    }

    // Process else branch (unwrap else_clause to get its body)
    TSNode alternative = ts_node_child_by_field_name(ifStmt, "alternative", 11);
    if (!ts_node_is_null(alternative) &&
        strcmp(ts_node_type(alternative), "else_clause") == 0)
    {
        alternative = ts_node_named_child(alternative, 0);
    }
    if (!ts_node_is_null(alternative))
    {
        auto elseResult = processStatement(alternative, file, func, bb);
        if (elseResult.first)
        {
            addIntraEdge(condNode, elseResult.first);
            if (elseResult.last) addIntraEdge(elseResult.last, mergeNode);
        }
        else
        {
            addIntraEdge(condNode, mergeNode);
        }
    }
    else
    {
        addIntraEdge(condNode, mergeNode);
    }

    return {firstNode, mergeNode};
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processWhileStmt(
    TSNode whileStmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    // Condition node
    IntraICFGNode* condNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(condNode);
    condNode->setSourceLoc(CTSParser::formatSourceLoc(whileStmt, file->getFilePath()));
    recordStmtNode(whileStmt, file, condNode);

    // After-loop node
    IntraICFGNode* afterNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(afterNode);

    // Process body
    TSNode body = CTSParser::getLoopBody(whileStmt);
    if (!ts_node_is_null(body))
    {
        auto bodyResult = processStatement(body, file, func, bb);
        if (bodyResult.first)
        {
            addIntraEdge(condNode, bodyResult.first);
            if (bodyResult.last) addIntraEdge(bodyResult.last, condNode); // back-edge
        }
    }

    addIntraEdge(condNode, afterNode); // loop exit

    return {condNode, afterNode};
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processForStmt(
    TSNode forStmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    // Init node
    IntraICFGNode* initNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(initNode);
    initNode->setSourceLoc(CTSParser::formatSourceLoc(forStmt, file->getFilePath()));
    recordStmtNode(forStmt, file, initNode);

    // Also record the init statement (e.g. declaration) to the same ICFG node
    TSNode initStmt = ts_node_child_by_field_name(forStmt, "initializer", 11);
    if (!ts_node_is_null(initStmt))
    {
        recordStmtNode(initStmt, file, initNode);
    }

    // Condition node
    IntraICFGNode* condNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(condNode);
    addIntraEdge(initNode, condNode);

    // After-loop node
    IntraICFGNode* afterNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(afterNode);

    // Update node
    IntraICFGNode* updateNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(updateNode);

    // Process body
    TSNode body = CTSParser::getLoopBody(forStmt);
    if (!ts_node_is_null(body))
    {
        auto bodyResult = processStatement(body, file, func, bb);
        if (bodyResult.first)
        {
            addIntraEdge(condNode, bodyResult.first);
            if (bodyResult.last) addIntraEdge(bodyResult.last, updateNode);
            addIntraEdge(updateNode, condNode); // back-edge
        }
    }
    else
    {
        addIntraEdge(condNode, updateNode);
        addIntraEdge(updateNode, condNode);
    }

    addIntraEdge(condNode, afterNode); // loop exit

    return {initNode, afterNode};
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processSwitchStmt(
    TSNode switchStmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    // Condition node
    IntraICFGNode* condNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(condNode);
    condNode->setSourceLoc(CTSParser::formatSourceLoc(switchStmt, file->getFilePath()));
    recordStmtNode(switchStmt, file, condNode);

    // After-switch node
    IntraICFGNode* afterNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(afterNode);

    // Process each case's body statements
    TSNode body = ts_node_child_by_field_name(switchStmt, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_named_child(body, i);
            if (strcmp(ts_node_type(child), "case_statement") == 0)
            {
                // Process statements in this case
                ICFGNodePair caseResult = {nullptr, nullptr};
                uint32_t caseCount = ts_node_named_child_count(child);
                for (uint32_t j = 0; j < caseCount; j++)
                {
                    TSNode caseChild = ts_node_named_child(child, j);
                    const char* ccType = ts_node_type(caseChild);
                    if (strcmp(ccType, "break_statement") == 0) continue;
                    auto stmtResult = processStatement(caseChild, file, func, bb);
                    if (stmtResult.first)
                    {
                        if (!caseResult.first) caseResult.first = stmtResult.first;
                        if (caseResult.last && caseResult.last != stmtResult.first)
                            addIntraEdge(caseResult.last, stmtResult.first);
                        caseResult.last = stmtResult.last;
                    }
                }
                if (caseResult.first)
                {
                    addIntraEdge(condNode, caseResult.first);
                    if (caseResult.last) addIntraEdge(caseResult.last, afterNode);
                }
            }
        }
    }

    // Fallthrough: cond -> after (for default/no-match)
    addIntraEdge(condNode, afterNode);

    return {condNode, afterNode};
}

CTSICFGBuilder::ICFGNodePair CTSICFGBuilder::processDoWhileStmt(
    TSNode doStmt, CTSSourceFile* file, const FunObjVar* func, const SVFBasicBlock* bb)
{
    // Body-start node
    IntraICFGNode* bodyStart = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(bodyStart);
    bodyStart->setSourceLoc(CTSParser::formatSourceLoc(doStmt, file->getFilePath()));
    recordStmtNode(doStmt, file, bodyStart);

    // After-loop node
    IntraICFGNode* afterNode = addIntraICFGNode(bb, false);
    const_cast<SVFBasicBlock*>(bb)->addICFGNode(afterNode);

    // Process body
    TSNode body = ts_node_named_child(doStmt, 0);
    if (!ts_node_is_null(body))
    {
        auto bodyResult = processStatement(body, file, func, bb);
        if (bodyResult.first)
        {
            addIntraEdge(bodyStart, bodyResult.first);
            // Back-edge: body end → body start (loop)
            if (bodyResult.last) addIntraEdge(bodyResult.last, bodyStart);
        }
    }

    addIntraEdge(bodyStart, afterNode); // loop exit

    return {bodyStart, afterNode};
}

bool CTSICFGBuilder::isReturnStmt(TSNode node) const
{
    if (ts_node_is_null(node)) return false;
    return strcmp(ts_node_type(node), "return_statement") == 0;
}

bool CTSICFGBuilder::containsCall(TSNode node, CTSSourceFile* file) const
{
    if (ts_node_is_null(node)) return false;
    if (strcmp(ts_node_type(node), "call_expression") == 0) return true;

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++)
    {
        if (containsCall(ts_node_child(node, i), file)) return true;
    }
    return false;
}

void CTSICFGBuilder::recordStmtNode(TSNode node, CTSSourceFile* file, ICFGNode* icfgNode)
{
    auto key = std::make_pair(file, ts_node_start_byte(node));
    stmtToICFGNode[key] = icfgNode;
}

void CTSICFGBuilder::setEdgeCondition(IntraCFGEdge* edge, const SVFVar* condVar, s64_t branchVal)
{
    edge->setConditionVar(condVar);
    edge->setBranchCondVal(branchVal);
}

ICFGNode* CTSICFGBuilder::getStmtICFGNode(TSNode node, CTSSourceFile* file) const
{
    auto key = std::make_pair(file, ts_node_start_byte(node));
    auto it = stmtToICFGNode.find(key);
    return (it != stmtToICFGNode.end()) ? it->second : nullptr;
}

const SVFBasicBlock* CTSICFGBuilder::getStmtBB(TSNode node, CTSSourceFile* file) const
{
    auto key = std::make_pair(file, ts_node_start_byte(node));
    auto it = stmtToBB.find(key);
    return (it != stmtToBB.end()) ? it->second : nullptr;
}

TSNode CTSICFGBuilder::findDirectCallExpr(TSNode stmt) const
{
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    if (ts_node_is_null(stmt)) return nullNode;

    const char* type = ts_node_type(stmt);

    // expression_statement → check if first named child is call_expression
    if (strcmp(type, "expression_statement") == 0)
    {
        TSNode child = ts_node_named_child(stmt, 0);
        if (!ts_node_is_null(child) && strcmp(ts_node_type(child), "call_expression") == 0)
            return child;
        // Also check assignment: x = foo() — the call is in the right side
        if (!ts_node_is_null(child) && strcmp(ts_node_type(child), "assignment_expression") == 0)
        {
            TSNode right = ts_node_child_by_field_name(child, "right", 5);
            if (!ts_node_is_null(right) && strcmp(ts_node_type(right), "call_expression") == 0)
                return right;
        }
    }

    // declaration with init: int x = foo()
    if (strcmp(type, "declaration") == 0)
    {
        TSNode declarator = CTSParser::getDeclarator(stmt);
        if (!ts_node_is_null(declarator) && CTSParser::isInitDeclarator(declarator))
        {
            TSNode init = CTSParser::getInitializer(declarator);
            if (!ts_node_is_null(init) && strcmp(ts_node_type(init), "call_expression") == 0)
                return init;
        }
    }

    // return foo()
    if (strcmp(type, "return_statement") == 0)
    {
        TSNode retVal = CTSParser::getReturnValue(stmt);
        if (!ts_node_is_null(retVal) && strcmp(ts_node_type(retVal), "call_expression") == 0)
            return retVal;
    }

    return nullNode;
}

void CTSICFGBuilder::findAllCallExprs(TSNode node, std::vector<TSNode>& results) const
{
    if (ts_node_is_null(node)) return;
    // Recurse into children first (bottom-up: inner calls collected before outer)
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++)
        findAllCallExprs(ts_node_child(node, i), results);
    // Then add self if call_expression
    if (strcmp(ts_node_type(node), "call_expression") == 0)
        results.push_back(node);
}

const FunObjVar* CTSICFGBuilder::lookupCallee(TSNode funcNode, CTSSourceFile* file) const
{
    if (ts_node_is_null(funcNode)) return nullptr;
    if (strcmp(ts_node_type(funcNode), "identifier") != 0) return nullptr;

    std::string funcName = CTSParser::getNodeText(funcNode, file->getSource());
    CTSFunction* func = moduleSet->getFunction(funcName);
    if (func) return func->getFunObjVar();
    return nullptr;
}

} // namespace SVF
