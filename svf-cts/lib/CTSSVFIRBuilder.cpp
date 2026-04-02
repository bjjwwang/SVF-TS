#include "CTS/CTSSVFIRBuilder.h"
#include "CTS/CTSParser.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/ObjTypeInfo.h"
#include "Graphs/ICFGNode.h"
#include "Graphs/BasicBlockG.h"
#include "Graphs/CHG.h"
#include "Graphs/CallGraph.h"
#include "Util/CallGraphBuilder.h"
#include "Util/NodeIDAllocator.h"
#include "Util/SVFLoopAndDomInfo.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

namespace SVF
{

CTSSVFIRBuilder::CTSSVFIRBuilder()
    : pag(nullptr), moduleSet(nullptr), icfgBuilder(nullptr),
      scopeManager(std::make_unique<ScopeManager>()),
      ssaBuilder(std::make_unique<SSABuilder>()),
      currentFunc(nullptr), currentBB(nullptr), currentICFGNode(nullptr),
      blackHoleNode((NodeID)-1), constPtrNode((NodeID)-1), nullPtrNode((NodeID)-1)
{
}

CTSSVFIRBuilder::~CTSSVFIRBuilder()
{
}

SVFIR* CTSSVFIRBuilder::build(const std::vector<std::string>& sourceFiles)
{
    // Step 1: Parse source files and collect symbols
    moduleSet = CTSModuleSet::getModuleSet();
    moduleSet->buildFromFiles(sourceFiles);

    // Step 1b: Build SVFStructTypes bottom-up with real field types
    // (must happen before PAG so resolveType works during createFunctionObjects)
    // Note: buildStructTypes needs 'pag' for StInfo, so we do type building in two steps
    buildStructTypes();

    // Step 2: Get SVFIR singleton (must be done before any node/edge creation)
    pag = SVFIR::getPAG();

    // Step 2b: Create StInfo for all types (needs pag for addStInfo)
    buildTypeInfo();

    // Step 3: Initialize special nodes
    initSpecialNodes();

    // Step 4: Create FunObjVar for all functions (before ICFG)
    createFunctionObjects();

    // Step 5: Build ICFG (needs FunObjVar to exist)
    icfgBuilder = new CTSICFGBuilder();
    ICFG* icfg = icfgBuilder->build(moduleSet);

    // Step 5b: Set module identifier (used by stats/analysis passes)
    if (!sourceFiles.empty())
        pag->setModuleIdentifier(sourceFiles[0]);

    // Step 6: Attach ICFG to SVFIR
    pag->setICFG(icfg);

    // Step 6b: Create empty CHG (no class hierarchy in C)
    CHGraph* chg = new CHGraph();
    pag->setCHG(chg);

    // Step 6c: Create return nodes for ALL functions (needs ICFG)
    createReturnNodes();

    // Step 7: Set up SSA builder callback
    ssaBuilder->setCreateNodeFn([this](const std::string& /*varName*/) -> NodeID {
        return createValNode(moduleSet->getPtrType(), currentICFGNode);
    });

    // Step 8: Process globals
    processGlobalVars();

    // Step 9: Process each function (creates CallPE/RetPE edges)
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;
        if (func->hasBody())
        {
            processFunction(func);
        }
    }

    // Step 9b: Register StInfo for any types created during function processing
    // (e.g., array types from local variable declarations)
    for (SVFType* type : moduleSet->getOwnedTypes())
    {
        if (type && type2StInfo.find(type) == type2StInfo.end())
            collectTypeInfo(type);
    }

    // Step 10: Build CallGraph (AFTER function processing, so CallPE edges exist)
    {
        std::vector<const FunObjVar*> funset;
        for (auto& pair : moduleSet->getFunctions())
        {
            CTSFunction* func = pair.second;
            if (func->getFunObjVar())
                funset.push_back(func->getFunObjVar());
        }
        CallGraphBuilder callGraphBuilder;
        CallGraph* cg = callGraphBuilder.buildSVFIRCallGraph(funset);
        pag->setCallGraph(cg);
    }

    std::cout << "SVFIR built: " << pag->getPAGNodeNum() << " nodes, "
              << pag->getPAGEdgeNum() << " edges" << std::endl;

    return pag;
}

//===----------------------------------------------------------------------===//
// Node creation
//===----------------------------------------------------------------------===//

NodeID CTSSVFIRBuilder::createLocalVar(const std::string& name, TSNode node,
                                        CTSSourceFile* file, const SVFType* type,
                                        const ICFGNode* icfgNode)
{
    // Create the stack object
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(type);
    ti->setFlag(ObjTypeInfo::STACK_OBJ);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addStackObjNode(objId, ti, icfgNode);
    pag->getGNode(objId)->setName(name);
    pag->getGNode(objId)->setSourceLoc(CTSParser::formatSourceLoc(node, file->getFilePath()));

    // Create the value node (pointer to the object)
    NodeID valId = NodeIDAllocator::get()->allocateValueId();
    pag->addValNode(valId, moduleSet->getPtrType(), icfgNode);
    pag->getGNode(valId)->setName(name);
    pag->getGNode(valId)->setSourceLoc(CTSParser::formatSourceLoc(node, file->getFilePath()));

    // Address edge: valId = &objId
    addAddrEdge(objId, valId);

    // Track in module
    moduleSet->setValID(node, file, valId);
    moduleSet->setObjID(node, file, objId);

    return valId;
}

NodeID CTSSVFIRBuilder::createGlobalVar(const std::string& name, TSNode node,
                                         CTSSourceFile* file, const SVFType* type)
{
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(type);
    ti->setFlag(ObjTypeInfo::GLOBVAR_OBJ);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addGlobalObjNode(objId, ti, nullptr);
    pag->getGNode(objId)->setName(name);
    pag->getGNode(objId)->setSourceLoc(CTSParser::formatSourceLoc(node, file->getFilePath()));

    NodeID valId = NodeIDAllocator::get()->allocateValueId();
    pag->addGlobalValNode(valId, nullptr, moduleSet->getPtrType());
    pag->getGNode(valId)->setName(name);
    pag->getGNode(valId)->setSourceLoc(CTSParser::formatSourceLoc(node, file->getFilePath()));

    addAddrEdge(objId, valId);

    moduleSet->setValID(node, file, valId);
    moduleSet->setObjID(node, file, objId);

    return valId;
}

NodeID CTSSVFIRBuilder::createHeapObj(TSNode allocSite, CTSSourceFile* file,
                                       const ICFGNode* icfgNode)
{
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(moduleSet->getPtrType());
    ti->setFlag(ObjTypeInfo::HEAP_OBJ);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addHeapObjNode(objId, ti, icfgNode);
    pag->getGNode(objId)->setName("heap_obj");
    pag->getGNode(objId)->setSourceLoc(CTSParser::formatSourceLoc(allocSite, file->getFilePath()));

    NodeID valId = NodeIDAllocator::get()->allocateValueId();
    pag->addValNode(valId, moduleSet->getPtrType(), icfgNode);
    pag->getGNode(valId)->setName("malloc_result");
    pag->getGNode(valId)->setSourceLoc(CTSParser::formatSourceLoc(allocSite, file->getFilePath()));

    addAddrEdge(objId, valId);

    moduleSet->setObjID(allocSite, file, objId);
    return valId;
}

NodeID CTSSVFIRBuilder::createValNode(const SVFType* type, const ICFGNode* icfgNode)
{
    NodeID id = NodeIDAllocator::get()->allocateValueId();
    pag->addValNode(id, type ? type : moduleSet->getPtrType(), icfgNode);
    return id;
}

NodeID CTSSVFIRBuilder::createConstIntNode(s64_t value, const ICFGNode* icfgNode)
{
    // Create ObjVar + ValVar + AddrStmt (matching LLVM pattern)
    // The AE's initObjVar needs the ObjVar to initialize constant values
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(moduleSet->getIntType());
    ti->setFlag(ObjTypeInfo::CONST_DATA);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addConstantIntObjNode(objId, ti, std::make_pair(value, (u64_t)value), icfgNode);

    NodeID valId = NodeIDAllocator::get()->allocateValueId();
    pag->addConstantIntValNode(valId, std::make_pair(value, (u64_t)value),
                                icfgNode, moduleSet->getIntType());

    // AddrStmt links obj → val, allowing AE to initialize the constant value
    addAddrEdge(objId, valId);

    return valId;
}

NodeID CTSSVFIRBuilder::createConstNullNode(const ICFGNode* icfgNode)
{
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(moduleSet->getPtrType());
    ti->setFlag(ObjTypeInfo::CONST_DATA);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addConstantNullPtrObjNode(objId, ti, icfgNode);

    NodeID valId = NodeIDAllocator::get()->allocateValueId();
    pag->addConstantNullPtrValNode(valId, icfgNode, moduleSet->getPtrType());

    addAddrEdge(objId, valId);

    return valId;
}

//===----------------------------------------------------------------------===//
// Edge creation (via friend access)
//===----------------------------------------------------------------------===//

void CTSSVFIRBuilder::addAddrEdge(NodeID src, NodeID dst)
{
    AddrStmt* edge = pag->addAddrStmt(src, dst);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addCopyEdge(NodeID src, NodeID dst)
{
    if (src == dst) return;
    CopyStmt* edge = pag->addCopyStmt(src, dst, CopyStmt::COPYVAL);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addLoadEdge(NodeID src, NodeID dst)
{
    LoadStmt* edge = pag->addLoadStmt(src, dst);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addStoreEdge(NodeID src, NodeID dst, const ICFGNode* icfgNode)
{
    StoreStmt* edge = pag->addStoreStmt(src, dst, icfgNode);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        ICFGNode* targetNode = const_cast<ICFGNode*>(icfgNode ? icfgNode : currentICFGNode);
        if (targetNode)
        {
            pag->addToSVFStmtList(targetNode, edge);
            targetNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addGepEdge(NodeID src, NodeID dst, const AccessPath& ap, bool constGep)
{
    GepStmt* edge = pag->addGepStmt(src, dst, ap, constGep);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addCallEdge(NodeID src, NodeID dst, const CallICFGNode* cs,
                                   const FunEntryICFGNode* entry)
{
    pag->addCallPE(src, dst, cs, entry);
}

void CTSSVFIRBuilder::addRetEdge(NodeID src, NodeID dst, const CallICFGNode* cs,
                                  const FunExitICFGNode* exit)
{
    pag->addRetPE(src, dst, cs, exit);
}

void CTSSVFIRBuilder::addBinaryOPEdge(NodeID op1, NodeID op2, NodeID dst, u32_t opcode)
{
    BinaryOPStmt* edge = pag->addBinaryOPStmt(op1, op2, dst, opcode);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        edge->setBB(currentBB);
        edge->setICFGNode(currentICFGNode);
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addCmpEdge(NodeID op1, NodeID op2, NodeID dst, u32_t predicate)
{
    CmpStmt* edge = pag->addCmpStmt(op1, op2, dst, predicate);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        edge->setBB(currentBB);
        edge->setICFGNode(currentICFGNode);
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addUnaryOPEdge(NodeID src, NodeID dst, u32_t opcode)
{
    UnaryOPStmt* edge = pag->addUnaryOPStmt(src, dst, opcode);
    if (edge)
    {
        edge->setValue(pag->getGNode(dst));
        edge->setBB(currentBB);
        edge->setICFGNode(currentICFGNode);
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
    }
}

void CTSSVFIRBuilder::addBranchEdge(NodeID br, NodeID cond,
                                     const BranchStmt::SuccAndCondPairVec& succs)
{
    BranchStmt* edge = pag->addBranchStmt(br, cond, succs);
    if (edge)
    {
        edge->setValue(pag->getGNode(cond));
        edge->setBB(currentBB);
        edge->setICFGNode(currentICFGNode);
    }
}

//===----------------------------------------------------------------------===//
// Build pipeline
//===----------------------------------------------------------------------===//

void CTSSVFIRBuilder::initSpecialNodes()
{
    blackHoleNode = pag->getBlackHoleNode();
    constPtrNode = pag->addDummyObjNode(moduleSet->getPtrType());
    nullPtrNode = pag->addDummyValNode();
}

void CTSSVFIRBuilder::createFunctionObjects()
{
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;

        NodeID objId = NodeIDAllocator::get()->allocateObjectId();

        // Create ObjTypeInfo for function
        const SVFType* funcType = moduleSet->getPtrType();
        ObjTypeInfo* ti = pag->createObjTypeInfo(funcType);
        ti->setFlag(ObjTypeInfo::FUNCTION_OBJ);
        pag->idToObjTypeInfoMap()[objId] = ti;
        pag->addFunObjNode(objId, ti, nullptr);

        FunObjVar* funObj = SVFUtil::cast<FunObjVar>(pag->getGNode(objId));
        funObj->setName(func->getName());
        funObj->setSourceLoc(CTSParser::formatSourceLoc(func->getNode(), func->getSourceFile()->getFilePath()));

        // Create BasicBlockGraph for ALL functions (needed for ICFG entry/exit nodes)
        BasicBlockGraph* bbGraph = new BasicBlockGraph();
        auto* entryBB = new SVFBasicBlock(1, funObj);
        entryBB->setName(func->getName() + ".entry");
        bbGraph->addBasicBlock(entryBB);
        auto* exitBB = new SVFBasicBlock(2, funObj);
        exitBB->setName(func->getName() + ".exit");
        bbGraph->addBasicBlock(exitBB);
        const SVFBasicBlock* exitBlock = exitBB;

        // Create SVFFunctionType (return type + param types)
        std::vector<const SVFType*> paramTypes;
        for (size_t i = 0; i < func->getParams().size(); i++)
        {
            paramTypes.push_back(moduleSet->getPtrType());
        }
        auto* svfFuncType = new SVFFunctionType(
            moduleSet->getTypeIdCounter(), moduleSet->getPtrType(), paramTypes, false);
        moduleSet->addOwnedType(svfFuncType);

        // Initialize FunObjVar with all required fields
        SVFLoopAndDomInfo* loopDom = new SVFLoopAndDomInfo();
        std::vector<const ArgValVar*> emptyArgs;
        funObj->initFunObjVar(
            !func->hasBody(),  // isDecl
            false,             // intrinsic
            false,             // isAddrTaken
            false,             // isUncalled
            false,             // isNotRet
            false,             // supVarArg
            svfFuncType,       // funcType
            loopDom,           // loopAndDom
            funObj,            // realDefFun (self)
            bbGraph,           // bbGraph
            emptyArgs,         // allArgs (will be populated later)
            exitBlock          // exitBlock
        );

        func->setFunObjVar(funObj);

        // Create ArgValVar for ALL functions (needed for CallPE edges)
        const auto& params = func->getParams();
        for (u32_t i = 0; i < params.size(); i++)
        {
            TSNode param = params[i];
            CTSSourceFile* file = func->getSourceFile();

            std::string paramName;
            TSNode paramDeclarator = CTSParser::getDeclarator(param);
            if (!ts_node_is_null(paramDeclarator))
                paramName = CTSParser::getDeclaratorName(paramDeclarator, file->getSource());
            if (paramName.empty())
                paramName = "arg" + std::to_string(i);

            const SVFType* paramType = resolveType(
                CTSParser::getTypeSpecifier(param), file);

            NodeID argId = NodeIDAllocator::get()->allocateValueId();
            pag->addArgValNode(argId, i, nullptr, funObj,
                               paramType ? paramType : moduleSet->getPtrType());
            pag->getGNode(argId)->setName(paramName);
            pag->getGNode(argId)->setSourceLoc(
                CTSParser::formatSourceLoc(param, file->getFilePath()));

            const ArgValVar* argValVar = SVFUtil::cast<ArgValVar>(pag->getGNode(argId));
            const_cast<FunObjVar*>(funObj)->addArgument(argValVar);
        }

        // Create function value node
        NodeID funValId = NodeIDAllocator::get()->allocateValueId();
        pag->addFunValNode(funValId, nullptr, funObj, moduleSet->getPtrType());
    }
}

void CTSSVFIRBuilder::createReturnNodes()
{
    ICFG* icfg = pag->getICFG();
    for (auto& pair : moduleSet->getFunctions())
    {
        CTSFunction* func = pair.second;
        const FunObjVar* funObj = func->getFunObjVar();
        if (!funObj) continue;

        FunExitICFGNode* exitNode = icfg->getFunExitICFGNode(funObj);
        if (!exitNode) continue;

        NodeID retId = NodeIDAllocator::get()->allocateValueId();
        pag->addRetNode(retId, funObj, moduleSet->getPtrType(), exitNode);
        pag->getGNode(retId)->setName("ret_" + func->getName());
        const SVFVar* retVar = pag->getGNode(retId);
        pag->addFunRet(funObj, retVar);
    }
}

void CTSSVFIRBuilder::processGlobalVars()
{
    // Pass 1: Create all global objects (so cross-references work)
    std::vector<std::pair<CTSGlobalVar*, NodeID>> globalEntries;
    for (auto& pair : moduleSet->getGlobalVars())
    {
        CTSGlobalVar* gvar = pair.second;
        const SVFType* type = resolveType(gvar->getTypeNode(), gvar->getSourceFile());
        NodeID valId = createGlobalVar(gvar->getName(), gvar->getNode(),
                                        gvar->getSourceFile(), type);
        globalEntries.push_back({gvar, valId});
    }

    // Pass 2: Process initializers (all globals now have valIds)
    for (auto& entry : globalEntries)
    {
        CTSGlobalVar* gvar = entry.first;
        NodeID valId = entry.second;
        TSNode init = gvar->getInitializer();
        if (!ts_node_is_null(init))
        {
            NodeID initVal = getExprValue(init, gvar->getSourceFile());
            if (initVal != blackHoleNode)
            {
                addStoreEdge(initVal, valId, nullptr);
            }
        }
    }
}

void CTSSVFIRBuilder::processFunction(CTSFunction* func)
{
    currentFunc = func;
    scopeManager->pushScope();
    ssaBuilder->clear();

    const FunObjVar* funObj = func->getFunObjVar();
    CTSSourceFile* file = func->getSourceFile();

    // Use existing ArgValVar nodes (created in createFunctionObjects())
    // and create stack copies for local scope
    const auto& params = func->getParams();
    for (u32_t i = 0; i < params.size() && i < funObj->arg_size(); i++)
    {
        TSNode param = params[i];
        const ArgValVar* argValVar = funObj->getArg(i);
        NodeID argId = argValVar->getId();

        std::string paramName = CTSParser::getDeclaratorName(
            CTSParser::getDeclarator(param), file->getSource());
        if (paramName.empty())
            paramName = "arg" + std::to_string(i);

        const SVFType* paramType = resolveType(
            CTSParser::getTypeSpecifier(param), file);

        // Create stack object for the parameter (local copy)
        NodeID objId = NodeIDAllocator::get()->allocateObjectId();
        ObjTypeInfo* ti = pag->createObjTypeInfo(paramType ? paramType : moduleSet->getPtrType());
        ti->setFlag(ObjTypeInfo::STACK_OBJ);
        pag->idToObjTypeInfoMap()[objId] = ti;
        pag->addStackObjNode(objId, ti, nullptr);
        pag->getGNode(objId)->setName(paramName + "_obj");
        pag->getGNode(objId)->setSourceLoc(CTSParser::formatSourceLoc(param, file->getFilePath()));

        // Address edge: local = &obj
        NodeID localVal = createValNode(moduleSet->getPtrType(), nullptr);
        pag->getGNode(localVal)->setName(paramName + "_addr");
        pag->getGNode(localVal)->setSourceLoc(CTSParser::formatSourceLoc(param, file->getFilePath()));
        addAddrEdge(objId, localVal);

        // Store argument into local: *localVal = argId
        addStoreEdge(argId, localVal, nullptr);

        // Register in scope
        scopeManager->declareVar(paramName, localVal, objId, paramType);

        // Register as function argument in PAG
        pag->addFunArgs(funObj, pag->getGNode(argId));
    }

    // Return node already created by createReturnNodes()

    // Process function body
    TSNode body = func->getBody();
    if (!ts_node_is_null(body))
    {
        processCompoundStmt(body, file);
    }

    scopeManager->popScope();
    currentFunc = nullptr;
}

void CTSSVFIRBuilder::processCompoundStmt(TSNode compound, CTSSourceFile* file)
{
    scopeManager->pushScope();

    uint32_t count = ts_node_named_child_count(compound);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(compound, i);
        processStatement(child, file);
    }

    scopeManager->popScope();
}

void CTSSVFIRBuilder::processStatement(TSNode stmt, CTSSourceFile* file)
{
    if (ts_node_is_null(stmt)) return;

    // Update current ICFG node
    currentICFGNode = getICFGNode(stmt, file);

    const char* type = ts_node_type(stmt);

    if (strcmp(type, "declaration") == 0)
    {
        processDeclaration(stmt, file);
    }
    else if (strcmp(type, "expression_statement") == 0)
    {
        TSNode expr = ts_node_named_child(stmt, 0);
        processExpression(expr, file);
    }
    else if (strcmp(type, "return_statement") == 0)
    {
        processReturn(stmt, file);
    }
    else if (strcmp(type, "if_statement") == 0)
    {
        processIfStatement(stmt, file);
    }
    else if (strcmp(type, "while_statement") == 0)
    {
        processWhileStatement(stmt, file);
    }
    else if (strcmp(type, "for_statement") == 0)
    {
        processForStatement(stmt, file);
    }
    else if (strcmp(type, "switch_statement") == 0)
    {
        processSwitchStatement(stmt, file);
    }
    else if (strcmp(type, "do_statement") == 0)
    {
        processDoWhileStatement(stmt, file);
    }
    else if (strcmp(type, "compound_statement") == 0)
    {
        processCompoundStmt(stmt, file);
    }
}

void CTSSVFIRBuilder::processDeclaration(TSNode decl, CTSSourceFile* file)
{
    const SVFType* baseType = resolveType(CTSParser::getTypeSpecifier(decl), file);

    // Handle all declarators in this declaration (int a, b, c = 1;)
    auto declarators = CTSParser::getDeclarators(decl);
    for (TSNode declarator : declarators)
    {
        std::string varName = CTSParser::getDeclaratorName(declarator, file->getSource());
        if (varName.empty()) continue;

        const SVFType* type = baseType;

        // Check if this is an array declaration (int arr[10])
        TSNode innerDecl = declarator;
        if (CTSParser::isInitDeclarator(declarator))
            innerDecl = ts_node_named_child(declarator, 0);
        if (!ts_node_is_null(innerDecl) &&
            strcmp(ts_node_type(innerDecl), "array_declarator") == 0)
        {
            unsigned arraySize = CTSParser::getArraySize(innerDecl, file->getSource());
            if (arraySize > 0 && type)
                type = moduleSet->createArrayType(type, arraySize);
        }

        NodeID valId = createLocalVar(varName, declarator, file,
                                       type ? type : moduleSet->getIntType(),
                                       currentICFGNode);

        // Create obj node for scope
        NodeID objId = moduleSet->getObjID(declarator, file);
        scopeManager->declareVar(varName, valId, objId, type);

        // Process initializer if present
        if (CTSParser::isInitDeclarator(declarator))
        {
            TSNode init = CTSParser::getInitializer(declarator);
            if (!ts_node_is_null(init))
            {
                NodeID initVal = getExprValue(init, file);
                if (initVal != blackHoleNode)
                {
                    addStoreEdge(initVal, valId, currentICFGNode);
                }
            }
        }
    }
}

void CTSSVFIRBuilder::processExpression(TSNode expr, CTSSourceFile* file)
{
    if (ts_node_is_null(expr)) return;

    const char* type = ts_node_type(expr);

    if (strcmp(type, "assignment_expression") == 0)
    {
        processAssignment(expr, file);
    }
    else if (strcmp(type, "call_expression") == 0)
    {
        processCallExpr(expr, file);
    }
    else if (strcmp(type, "update_expression") == 0)
    {
        // Handle ++/-- : load, add/sub 1, store back
        TSNode operand = ts_node_named_child(expr, 0);
        NodeID loadVal = getExprValue(operand, file);
        NodeID one = createConstIntNode(1, currentICFGNode);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);

        // Determine if ++ or --
        uint32_t childCount = ts_node_child_count(expr);
        bool isIncrement = true;
        for (uint32_t i = 0; i < childCount; i++)
        {
            TSNode child = ts_node_child(expr, i);
            std::string childText = CTSParser::getNodeText(child, file->getSource());
            if (childText == "--") { isIncrement = false; break; }
        }

        addBinaryOPEdge(loadVal, one, result,
                         isIncrement ? BinaryOPStmt::Add : BinaryOPStmt::Sub);

        // Store result back to lvalue
        NodeID lval = getExprLValue(operand, file);
        if (lval != blackHoleNode)
            addStoreEdge(result, lval, currentICFGNode);
    }
    else
    {
        // Evaluate for side effects
        getExprValue(expr, file);
    }
}

void CTSSVFIRBuilder::processAssignment(TSNode assign, CTSSourceFile* file)
{
    TSNode left = ts_node_child_by_field_name(assign, "left", 4);
    TSNode right = ts_node_child_by_field_name(assign, "right", 5);

    if (ts_node_is_null(left) || ts_node_is_null(right)) return;

    // Detect compound assignment operator (+=, -=, *=, etc.)
    // by checking the anonymous operator child (child index 1)
    bool isCompound = false;
    if (ts_node_child_count(assign) >= 3)
    {
        TSNode opNode = ts_node_child(assign, 1);
        std::string op = CTSParser::getNodeText(opNode, file->getSource());
        if (op != "=") isCompound = true;
    }

    NodeID rhsVal = getExprValue(right, file);

    // For compound assignment (a += 2), read-modify-write:
    // load a, compute (a op 2), store result back
    if (isCompound)
    {
        // Load current value of LHS
        NodeID lhsLoadVal = getExprValue(left, file);

        // Determine the binary op from the compound operator
        TSNode opNode = ts_node_child(assign, 1);
        std::string op = CTSParser::getNodeText(opNode, file->getSource());
        NodeID compoundResult = createValNode(moduleSet->getPtrType(), currentICFGNode);

        if (op == "+=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Add);
        else if (op == "-=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Sub);
        else if (op == "*=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Mul);
        else if (op == "/=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::SDiv);
        else if (op == "%=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::SRem);
        else if (op == "&=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::And);
        else if (op == "|=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Or);
        else if (op == "^=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Xor);
        else if (op == "<<=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::Shl);
        else if (op == ">>=")
            addBinaryOPEdge(lhsLoadVal, rhsVal, compoundResult, BinaryOPStmt::AShr);
        else
            addCopyEdge(lhsLoadVal, compoundResult);

        rhsVal = compoundResult;
    }

    const char* leftType = ts_node_type(left);

    if (strcmp(leftType, "pointer_expression") == 0)
    {
        // *p = x → Store
        TSNode operand = ts_node_named_child(left, 0);
        NodeID ptrVal = getExprValue(operand, file);
        addStoreEdge(rhsVal, ptrVal, currentICFGNode);
    }
    else if (strcmp(leftType, "identifier") == 0)
    {
        // x = expr → Store to x's address
        std::string varName = CTSParser::getNodeText(left, file->getSource());
        auto* varInfo = scopeManager->lookupVar(varName);
        if (varInfo)
        {
            addStoreEdge(rhsVal, varInfo->valNode, currentICFGNode);
        }
        else
        {
            // Check global variables
            CTSGlobalVar* gvar = moduleSet->getGlobalVar(varName);
            if (gvar)
            {
                NodeID gvalId = moduleSet->getValID(gvar->getNode(), gvar->getSourceFile());
                if (gvalId != (NodeID)-1)
                    addStoreEdge(rhsVal, gvalId, currentICFGNode);
            }
        }
    }
    else if (strcmp(leftType, "field_expression") == 0 ||
             strcmp(leftType, "subscript_expression") == 0)
    {
        NodeID lhsAddr = getExprLValue(left, file);
        addStoreEdge(rhsVal, lhsAddr, currentICFGNode);
    }
    else
    {
        // Fallback
        NodeID lhsVal = getExprLValue(left, file);
        addStoreEdge(rhsVal, lhsVal, currentICFGNode);
    }
}

NodeID CTSSVFIRBuilder::processCallExpr(TSNode call, CTSSourceFile* file)
{
    TSNode funcNode = ts_node_child_by_field_name(call, "function", 8);
    if (ts_node_is_null(funcNode))
        return createValNode(moduleSet->getPtrType(), currentICFGNode);

    std::string funcName;
    if (strcmp(ts_node_type(funcNode), "identifier") == 0)
    {
        funcName = CTSParser::getNodeText(funcNode, file->getSource());
    }

    // Handle malloc/calloc as heap allocation
    if (funcName == "malloc" || funcName == "calloc")
    {
        return createHeapObj(call, file, currentICFGNode);
    }

    // Handle realloc as heap allocation
    if (funcName == "realloc")
    {
        return createHeapObj(call, file, currentICFGNode);
    }

    // Handle free as no-op (returns void)
    if (funcName == "free")
    {
        // Still evaluate args for side effects
        TSNode args = ts_node_child_by_field_name(call, "arguments", 9);
        if (!ts_node_is_null(args))
        {
            uint32_t argCount = ts_node_named_child_count(args);
            for (uint32_t i = 0; i < argCount; i++)
                getExprValue(ts_node_named_child(args, i), file);
        }
        return createValNode(moduleSet->getPtrType(), currentICFGNode);
    }

    // Look up callee
    CTSFunction* calleeFunc = moduleSet->getFunction(funcName);
    const FunObjVar* calleeFunObj = calleeFunc ? calleeFunc->getFunObjVar() : nullptr;

    // Evaluate arguments
    std::vector<NodeID> argVals;
    TSNode args = ts_node_child_by_field_name(call, "arguments", 9);
    if (!ts_node_is_null(args))
    {
        uint32_t argCount = ts_node_named_child_count(args);
        for (uint32_t i = 0; i < argCount; i++)
        {
            TSNode arg = ts_node_named_child(args, i);
            argVals.push_back(getExprValue(arg, file));
        }
    }

    // Look up the CallICFGNode specifically for THIS call_expression (by byte offset)
    ICFGNode* callNodeForThis = icfgBuilder->getStmtICFGNode(call, file);
    CallICFGNode* callICFGNode = callNodeForThis
        ? SVFUtil::dyn_cast<CallICFGNode>(callNodeForThis)
        : SVFUtil::dyn_cast<CallICFGNode>(currentICFGNode);

    if (callICFGNode)
    {
        // Save and update currentICFGNode so node creation attaches to the right ICFG node
        ICFGNode* savedICFGNode = currentICFGNode;
        currentICFGNode = callICFGNode;

        // Register call site
        pag->addCallSite(callICFGNode);

        // Add actual arguments to call site (for ALL functions, including externals)
        for (u32_t i = 0; i < argVals.size(); i++)
        {
            SVFVar* argNode = pag->getGNode(argVals[i]);
            if (SVFUtil::isa<ValVar>(argNode))
                pag->addCallSiteArgs(callICFGNode, SVFUtil::cast<ValVar>(argNode));
        }

        if (calleeFunObj)
        {
            ICFG* icfg = pag->getICFG();

            // CallPE: actual_arg[i] → formal_arg[i]
            FunEntryICFGNode* entry = icfg->getFunEntryICFGNode(calleeFunObj);
            if (entry)
            {
                u32_t minArgs = std::min((u32_t)argVals.size(), calleeFunObj->arg_size());
                for (u32_t i = 0; i < minArgs; i++)
                {
                    addCallEdge(argVals[i], calleeFunObj->getArg(i)->getId(),
                               callICFGNode, entry);
                }
            }

            // RetPE: callee_ret → call_result
            if (pag->funHasRet(calleeFunObj))
            {
                ICFG* icfg = pag->getICFG();
                FunExitICFGNode* exit = icfg->getFunExitICFGNode(calleeFunObj);
                NodeID resultNode = createValNode(moduleSet->getPtrType(), callICFGNode);
                addRetEdge(pag->getFunRet(calleeFunObj)->getId(), resultNode,
                          callICFGNode, exit);
                // Register return value in call site
                RetICFGNode* retICFGNode = const_cast<RetICFGNode*>(callICFGNode->getRetICFGNode());
                if (retICFGNode)
                    pag->addCallSiteRets(retICFGNode, pag->getGNode(resultNode));

                currentICFGNode = savedICFGNode;
                return resultNode;
            }
        }

        currentICFGNode = savedICFGNode;
    }

    return createValNode(moduleSet->getPtrType(), currentICFGNode);
}

void CTSSVFIRBuilder::processReturn(TSNode ret, CTSSourceFile* file)
{
    TSNode retVal = CTSParser::getReturnValue(ret);
    if (!ts_node_is_null(retVal) && currentFunc)
    {
        NodeID val = getExprValue(retVal, file);
        const FunObjVar* funObj = currentFunc->getFunObjVar();
        if (funObj && pag->funHasRet(funObj))
        {
            const SVFVar* retNode = pag->getFunRet(funObj);
            addCopyEdge(val, retNode->getId());
        }
    }
}

void CTSSVFIRBuilder::processIfStatement(TSNode ifStmt, CTSSourceFile* file)
{
    // Process condition and create condition variable for branch pruning
    TSNode cond = ts_node_child_by_field_name(ifStmt, "condition", 9);
    NodeID condVal = blackHoleNode;
    if (!ts_node_is_null(cond))
    {
        // Strip parenthesized_expression wrapper (tree-sitter wraps if-conditions)
        TSNode innerCond = cond;
        if (strcmp(ts_node_type(cond), "parenthesized_expression") == 0)
            innerCond = ts_node_named_child(cond, 0);
        condVal = getExprValue(innerCond, file);
    }

    // Set branch conditions on ICFG edges from the condition node
    ICFGNode* condICFGNode = getICFGNode(ifStmt, file);
    if (condICFGNode && condVal != blackHoleNode)
    {
        const SVFVar* condVar = pag->getGNode(condVal);

        // Collect distinct outgoing intra-edge destinations
        std::set<const ICFGNode*> dstNodes;
        for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
        {
            if (SVFUtil::isa<IntraCFGEdge>(*it))
                dstNodes.insert((*it)->getDstNode());
        }

        // Only set branch conditions if there are at least 2 distinct successors
        // (otherwise both edges go to the same merge node — no real branch)
        if (dstNodes.size() >= 2)
        {
            // Find the then-branch ICFG node: look for the first ICFG node
            // inside the consequence body
            TSNode consequence = ts_node_child_by_field_name(ifStmt, "consequence", 11);
            ICFGNode* thenICFGNode = nullptr;
            if (!ts_node_is_null(consequence))
            {
                thenICFGNode = getICFGNode(consequence, file);
                // If consequence is a compound_statement, try its first child
                if (!thenICFGNode && strcmp(ts_node_type(consequence), "compound_statement") == 0)
                {
                    uint32_t count = ts_node_named_child_count(consequence);
                    for (uint32_t i = 0; i < count && !thenICFGNode; i++)
                    {
                        TSNode child = ts_node_named_child(consequence, i);
                        thenICFGNode = getICFGNode(child, file);
                    }
                }
            }

            if (thenICFGNode)
            {
                for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
                {
                    if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                    {
                        if (edge->getDstNode() == thenICFGNode)
                            icfgBuilder->setEdgeCondition(edge, condVar, 1);
                        else
                            icfgBuilder->setEdgeCondition(edge, condVar, 0);
                    }
                }

                BranchStmt::SuccAndCondPairVec succs;
                for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
                {
                    if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                    {
                        const ICFGNode* dst = edge->getDstNode();
                        s32_t cv = (dst == thenICFGNode) ? 1 : 0;
                        succs.push_back(std::make_pair(dst, cv));
                    }
                }
                if (!succs.empty())
                    addBranchEdge(condVal, condVal, succs);
            }
            // If thenICFGNode is still null, don't set conditions — leave edges unconditional
        }
    }

    // Process then branch
    TSNode consequence2 = ts_node_child_by_field_name(ifStmt, "consequence", 11);
    if (!ts_node_is_null(consequence2))
    {
        processStatement(consequence2, file);
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
        processStatement(alternative, file);
    }
}

void CTSSVFIRBuilder::processWhileStatement(TSNode whileStmt, CTSSourceFile* file)
{
    TSNode cond = ts_node_child_by_field_name(whileStmt, "condition", 9);
    if (!ts_node_is_null(cond))
    {
        getExprValue(cond, file);
    }

    TSNode body = CTSParser::getLoopBody(whileStmt);
    if (!ts_node_is_null(body))
    {
        processStatement(body, file);
    }
}

void CTSSVFIRBuilder::processForStatement(TSNode forStmt, CTSSourceFile* file)
{
    TSNode init = ts_node_child_by_field_name(forStmt, "initializer", 11);
    if (!ts_node_is_null(init))
    {
        processStatement(init, file);
    }

    TSNode cond = ts_node_child_by_field_name(forStmt, "condition", 9);
    if (!ts_node_is_null(cond))
    {
        getExprValue(cond, file);
    }

    TSNode update = ts_node_child_by_field_name(forStmt, "update", 6);
    if (!ts_node_is_null(update))
    {
        processExpression(update, file);
    }

    TSNode body = CTSParser::getLoopBody(forStmt);
    if (!ts_node_is_null(body))
    {
        processStatement(body, file);
    }
}

void CTSSVFIRBuilder::processSwitchStatement(TSNode switchStmt, CTSSourceFile* file)
{
    // Process condition
    TSNode cond = ts_node_child_by_field_name(switchStmt, "condition", 9);
    if (!ts_node_is_null(cond))
    {
        getExprValue(cond, file);
    }

    // Process body (compound_statement containing case_statement nodes)
    TSNode body = ts_node_child_by_field_name(switchStmt, "body", 4);
    if (!ts_node_is_null(body))
    {
        uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_named_child(body, i);
            const char* childType = ts_node_type(child);
            if (strcmp(childType, "case_statement") == 0)
            {
                // Process statements inside case (skip the case value itself)
                uint32_t caseCount = ts_node_named_child_count(child);
                for (uint32_t j = 0; j < caseCount; j++)
                {
                    TSNode caseChild = ts_node_named_child(child, j);
                    const char* ccType = ts_node_type(caseChild);
                    // Skip case value (number_literal etc.) and break_statement
                    if (strcmp(ccType, "break_statement") == 0) continue;
                    if (strcmp(ccType, "expression_statement") == 0 ||
                        strcmp(ccType, "declaration") == 0 ||
                        strcmp(ccType, "return_statement") == 0 ||
                        strcmp(ccType, "compound_statement") == 0)
                    {
                        processStatement(caseChild, file);
                    }
                }
            }
        }
    }
}

void CTSSVFIRBuilder::processDoWhileStatement(TSNode doStmt, CTSSourceFile* file)
{
    // Process body
    TSNode body = ts_node_named_child(doStmt, 0);
    if (!ts_node_is_null(body))
    {
        processStatement(body, file);
    }

    // Process condition
    TSNode cond = ts_node_child_by_field_name(doStmt, "condition", 9);
    if (!ts_node_is_null(cond))
    {
        getExprValue(cond, file);
    }
}

//===----------------------------------------------------------------------===//
// Expression evaluation
//===----------------------------------------------------------------------===//

NodeID CTSSVFIRBuilder::getExprValue(TSNode expr, CTSSourceFile* file)
{
    if (ts_node_is_null(expr)) return blackHoleNode;

    const char* type = ts_node_type(expr);

    // true/false literals (tree-sitter parses these as identifiers without preprocessing)
    if (strcmp(type, "true") == 0)
        return createConstIntNode(1, currentICFGNode);
    if (strcmp(type, "false") == 0)
        return createConstIntNode(0, currentICFGNode);

    // Identifier → look up variable, load its value
    if (strcmp(type, "identifier") == 0)
    {
        std::string name = CTSParser::getNodeText(expr, file->getSource());

        // Handle true/false as identifiers (from stdbool.h without preprocessing)
        if (name == "true" || name == "TRUE")
            return createConstIntNode(1, currentICFGNode);
        if (name == "false" || name == "FALSE")
            return createConstIntNode(0, currentICFGNode);
        // Handle NULL as identifier
        if (name == "NULL")
            return createConstNullNode(currentICFGNode);

        auto* varInfo = scopeManager->lookupVar(name);
        if (varInfo)
        {
            // Load the value from the variable's address
            NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
            addLoadEdge(varInfo->valNode, result);
            return result;
        }
        // Check global variables
        CTSGlobalVar* gvar = moduleSet->getGlobalVar(name);
        if (gvar)
        {
            NodeID gvalId = moduleSet->getValID(gvar->getNode(), gvar->getSourceFile());
            if (gvalId != (NodeID)-1)
            {
                NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
                addLoadEdge(gvalId, result);
                return result;
            }
        }
        // Could be a function name
        CTSFunction* func = moduleSet->getFunction(name);
        if (func && func->getFunObjVar())
        {
            return func->getFunObjVar()->getId();
        }
        return blackHoleNode;
    }

    // Number literal
    if (strcmp(type, "number_literal") == 0)
    {
        std::string text = CTSParser::getNodeText(expr, file->getSource());
        s64_t val = 0;
        try {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                val = std::stoll(text, nullptr, 16);
            else if (text.size() > 1 && text[0] == '0' && text.find('.') == std::string::npos)
                val = std::stoll(text, nullptr, 8);
            else
                val = std::stoll(text);
        } catch (...) {}
        return createConstIntNode(val, currentICFGNode);
    }

    // Character literal: 'A' → integer value
    if (strcmp(type, "char_literal") == 0)
    {
        std::string text = CTSParser::getNodeText(expr, file->getSource());
        s64_t val = 0;
        if (text.size() >= 3 && text[0] == '\'' && text.back() == '\'')
        {
            if (text[1] == '\\' && text.size() >= 4)
            {
                // Escape sequences
                switch (text[2])
                {
                    case 'n': val = '\n'; break;
                    case 't': val = '\t'; break;
                    case '0': val = '\0'; break;
                    case '\\': val = '\\'; break;
                    case '\'': val = '\''; break;
                    case '"': val = '"'; break;
                    case 'r': val = '\r'; break;
                    default: val = text[2]; break;
                }
            }
            else
            {
                val = (unsigned char)text[1];
            }
        }
        return createConstIntNode(val, currentICFGNode);
    }

    // String literal
    if (strcmp(type, "string_literal") == 0)
    {
        return constPtrNode;
    }

    // NULL / null
    if (strcmp(type, "null") == 0)
    {
        return createConstNullNode(currentICFGNode);
    }

    // Pointer expression: *p (dereference) or &x (address-of)
    if (strcmp(type, "pointer_expression") == 0)
    {
        // Check operator: tree-sitter-c uses pointer_expression for both * and &
        TSNode opNode = ts_node_child(expr, 0);
        std::string op = CTSParser::getNodeText(opNode, file->getSource());
        TSNode operand = ts_node_named_child(expr, 0);

        if (op == "&")
        {
            // Address-of: return the lvalue (pointer to the variable)
            if (strcmp(ts_node_type(operand), "identifier") == 0)
            {
                std::string name = CTSParser::getNodeText(operand, file->getSource());
                auto* varInfo = scopeManager->lookupVar(name);
                if (varInfo)
                    return varInfo->valNode;
                CTSGlobalVar* gvar = moduleSet->getGlobalVar(name);
                if (gvar)
                {
                    NodeID gvalId = moduleSet->getValID(gvar->getNode(), gvar->getSourceFile());
                    if (gvalId != (NodeID)-1)
                        return gvalId;
                }
            }
            return getExprLValue(operand, file);
        }
        else
        {
            // Dereference: *p → load from pointer
            NodeID ptrVal = getExprValue(operand, file);
            NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
            addLoadEdge(ptrVal, result);
            return result;
        }
    }

    // Address-of &x
    if (strcmp(type, "unary_expression") == 0)
    {
        TSNode op = ts_node_child(expr, 0);
        std::string opStr = CTSParser::getNodeText(op, file->getSource());
        if (opStr == "&")
        {
            TSNode operand = ts_node_named_child(expr, 0);
            // Return the address of the operand
            if (strcmp(ts_node_type(operand), "identifier") == 0)
            {
                std::string name = CTSParser::getNodeText(operand, file->getSource());
                auto* varInfo = scopeManager->lookupVar(name);
                if (varInfo)
                {
                    return varInfo->valNode;
                }
            }
            return getExprLValue(operand, file);
        }
        // Other unary operators (-, !, ~)
        TSNode operand = ts_node_named_child(expr, 0);
        NodeID operandVal = getExprValue(operand, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
        if (opStr == "-")
        {
            // Negate: 0 - operand
            NodeID zero = createConstIntNode(0, currentICFGNode);
            addBinaryOPEdge(zero, operandVal, result, BinaryOPStmt::Sub);
        }
        else if (opStr == "!")
        {
            // Logical NOT: operand == 0
            NodeID zero = createConstIntNode(0, currentICFGNode);
            addCmpEdge(operandVal, zero, result, CmpStmt::ICMP_EQ);
        }
        else if (opStr == "~")
        {
            // Bitwise NOT: operand ^ -1
            NodeID allOnes = createConstIntNode(-1, currentICFGNode);
            addBinaryOPEdge(operandVal, allOnes, result, BinaryOPStmt::Xor);
        }
        else
        {
            addCopyEdge(operandVal, result);
        }
        return result;
    }

    // Binary expression
    if (strcmp(type, "binary_expression") == 0)
    {
        TSNode left = ts_node_child_by_field_name(expr, "left", 4);
        TSNode right = ts_node_child_by_field_name(expr, "right", 5);
        NodeID lhsVal = getExprValue(left, file);
        NodeID rhsVal = getExprValue(right, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);

        // Get operator string
        TSNode opNode = ts_node_child(expr, 1);
        std::string op = CTSParser::getNodeText(opNode, file->getSource());

        // Comparison operators → CmpStmt
        if (op == "==")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_EQ);
        else if (op == "!=")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_NE);
        else if (op == ">")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_SGT);
        else if (op == ">=")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_SGE);
        else if (op == "<")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_SLT);
        else if (op == "<=")
            addCmpEdge(lhsVal, rhsVal, result, CmpStmt::ICMP_SLE);
        // Arithmetic operators → BinaryOPStmt
        else if (op == "+")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Add);
        else if (op == "-")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Sub);
        else if (op == "*")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Mul);
        else if (op == "/")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::SDiv);
        else if (op == "%")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::SRem);
        // Bitwise operators
        else if (op == "&")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::And);
        else if (op == "|")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Or);
        else if (op == "^")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Xor);
        else if (op == "<<")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Shl);
        else if (op == ">>")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::AShr);
        // Logical operators (&&, ||) — model as bitwise AND/OR of boolean values
        else if (op == "&&")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::And);
        else if (op == "||")
            addBinaryOPEdge(lhsVal, rhsVal, result, BinaryOPStmt::Or);
        else
        {
            // Unknown operator: just copy from lhs as fallback
            addCopyEdge(lhsVal, result);
        }

        return result;
    }

    // Call expression → evaluate and return result
    if (strcmp(type, "call_expression") == 0)
    {
        return processCallExpr(expr, file);
    }

    // Cast expression
    if (strcmp(type, "cast_expression") == 0)
    {
        TSNode value = ts_node_child_by_field_name(expr, "value", 5);
        NodeID srcVal = getExprValue(value, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
        addCopyEdge(srcVal, result);
        return result;
    }

    // Parenthesized expression
    if (strcmp(type, "parenthesized_expression") == 0)
    {
        TSNode inner = ts_node_named_child(expr, 0);
        return getExprValue(inner, file);
    }

    // Subscript (array access) a[i] — GEP + Load
    if (strcmp(type, "subscript_expression") == 0)
    {
        NodeID gepNode = getArrayGepNode(expr, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
        addLoadEdge(gepNode, result);
        return result;
    }

    // Field expression (s.f or s->f) — GEP + Load
    if (strcmp(type, "field_expression") == 0)
    {
        NodeID gepNode = getFieldGepNode(expr, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
        addLoadEdge(gepNode, result);
        return result;
    }

    // Sizeof expression
    if (strcmp(type, "sizeof_expression") == 0)
    {
        return createConstIntNode(8, currentICFGNode); // Simplified
    }

    // Assignment expression (when used as rvalue)
    if (strcmp(type, "assignment_expression") == 0)
    {
        processAssignment(expr, file);
        TSNode right = ts_node_child_by_field_name(expr, "right", 5);
        return getExprValue(right, file);
    }

    // Conditional expression (ternary)
    if (strcmp(type, "conditional_expression") == 0)
    {
        TSNode cond = ts_node_named_child(expr, 0);
        TSNode then_expr = ts_node_named_child(expr, 1);
        TSNode else_expr = ts_node_named_child(expr, 2);
        getExprValue(cond, file);
        getExprValue(then_expr, file);
        getExprValue(else_expr, file);
        return createValNode(moduleSet->getPtrType(), currentICFGNode);
    }

    // Comma expression
    if (strcmp(type, "comma_expression") == 0)
    {
        NodeID result = blackHoleNode;
        uint32_t count = ts_node_named_child_count(expr);
        for (uint32_t i = 0; i < count; i++)
        {
            result = getExprValue(ts_node_named_child(expr, i), file);
        }
        return result;
    }

    // Update expression (i++, i--, ++i, --i) used as rvalue
    if (strcmp(type, "update_expression") == 0)
    {
        TSNode operand = ts_node_named_child(expr, 0);
        NodeID loadVal = getExprValue(operand, file);
        NodeID one = createConstIntNode(1, currentICFGNode);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);

        uint32_t childCount = ts_node_child_count(expr);
        bool isIncrement = true;
        bool isPrefix = false;
        for (uint32_t i = 0; i < childCount; i++)
        {
            TSNode child = ts_node_child(expr, i);
            std::string childText = CTSParser::getNodeText(child, file->getSource());
            if (childText == "--") { isIncrement = false; break; }
        }
        // Check if prefix (operator before operand)
        if (childCount >= 2)
        {
            TSNode first = ts_node_child(expr, 0);
            std::string firstText = CTSParser::getNodeText(first, file->getSource());
            if (firstText == "++" || firstText == "--") isPrefix = true;
        }

        addBinaryOPEdge(loadVal, one, result,
                         isIncrement ? BinaryOPStmt::Add : BinaryOPStmt::Sub);

        // Store result back
        NodeID lval = getExprLValue(operand, file);
        if (lval != blackHoleNode)
            addStoreEdge(result, lval, currentICFGNode);

        // For prefix, return the new value; for postfix, return the old value
        return isPrefix ? result : loadVal;
    }

    // Initializer list {0, 1, 2} — just return first element value
    if (strcmp(type, "initializer_list") == 0)
    {
        uint32_t count = ts_node_named_child_count(expr);
        if (count > 0)
            return getExprValue(ts_node_named_child(expr, 0), file);
        return createConstIntNode(0, currentICFGNode);
    }

    // Default: create a value node
    return createValNode(moduleSet->getPtrType(), currentICFGNode);
}

NodeID CTSSVFIRBuilder::getExprLValue(TSNode expr, CTSSourceFile* file)
{
    if (ts_node_is_null(expr)) return blackHoleNode;

    const char* type = ts_node_type(expr);

    if (strcmp(type, "identifier") == 0)
    {
        std::string name = CTSParser::getNodeText(expr, file->getSource());
        auto* varInfo = scopeManager->lookupVar(name);
        if (varInfo) return varInfo->valNode;
        // Check global variables
        CTSGlobalVar* gvar = moduleSet->getGlobalVar(name);
        if (gvar)
        {
            NodeID gvalId = moduleSet->getValID(gvar->getNode(), gvar->getSourceFile());
            if (gvalId != (NodeID)-1) return gvalId;
        }
        return blackHoleNode;
    }

    if (strcmp(type, "pointer_expression") == 0)
    {
        TSNode operand = ts_node_named_child(expr, 0);
        return getExprValue(operand, file);
    }

    if (strcmp(type, "subscript_expression") == 0)
    {
        return getArrayGepNode(expr, file);
    }

    if (strcmp(type, "field_expression") == 0)
    {
        return getFieldGepNode(expr, file);
    }

    return getExprValue(expr, file);
}

//===----------------------------------------------------------------------===//
// Type resolution and StInfo
//===----------------------------------------------------------------------===//

const SVFType* CTSSVFIRBuilder::resolveType(TSNode typeSpec, CTSSourceFile* file)
{
    if (ts_node_is_null(typeSpec)) return moduleSet->getIntType();

    const char* nodeType = ts_node_type(typeSpec);

    // Primitive types: int, char, float, double, void, bool, etc.
    if (strcmp(nodeType, "primitive_type") == 0)
    {
        std::string text = CTSParser::getNodeText(typeSpec, file->getSource());
        return moduleSet->getOrCreateType(text);
    }

    // Struct specifier: "struct S" or "struct S { ... }"
    if (strcmp(nodeType, "struct_specifier") == 0)
    {
        std::string name = CTSParser::getStructName(typeSpec, file->getSource());
        if (!name.empty())
        {
            CTSStructDef* def = moduleSet->getStructDef(name);
            if (def && def->getSVFType()) return def->getSVFType();
        }
        return moduleSet->getOrCreateType("struct " + name);
    }

    // Sized type specifier: "unsigned int", "long long", etc.
    if (strcmp(nodeType, "sized_type_specifier") == 0)
    {
        std::string text = CTSParser::getNodeText(typeSpec, file->getSource());
        if (text.find("long") != std::string::npos) return moduleSet->getOrCreateType("long");
        if (text.find("short") != std::string::npos) return moduleSet->getOrCreateType("short");
        if (text.find("char") != std::string::npos) return moduleSet->getOrCreateType("char");
        return moduleSet->getIntType();
    }

    // type_identifier: typedef name
    if (strcmp(nodeType, "type_identifier") == 0)
    {
        std::string text = CTSParser::getNodeText(typeSpec, file->getSource());
        CTSStructDef* def = moduleSet->getStructDef(text);
        if (def && def->getSVFType()) return def->getSVFType();
        return moduleSet->getOrCreateType(text);
    }

    // Fallback: try text-based lookup
    std::string text = CTSParser::getNodeText(typeSpec, file->getSource());
    return moduleSet->getOrCreateType(text);
}

const SVFType* CTSSVFIRBuilder::resolveFullType(TSNode typeSpec, TSNode declarator,
                                                  CTSSourceFile* file)
{
    const SVFType* baseType = resolveType(typeSpec, file);

    TSNode inner = declarator;
    if (!ts_node_is_null(inner) && CTSParser::isInitDeclarator(inner))
        inner = ts_node_named_child(inner, 0);

    if (!ts_node_is_null(inner))
    {
        const char* dt = ts_node_type(inner);
        if (strcmp(dt, "pointer_declarator") == 0)
            return moduleSet->getPtrType();
        if (strcmp(dt, "array_declarator") == 0)
        {
            unsigned size = CTSParser::getArraySize(inner, file->getSource());
            if (size > 0 && baseType)
                return moduleSet->createArrayType(baseType, size);
        }
    }
    return baseType;
}

void CTSSVFIRBuilder::buildStructTypes()
{
    auto& structDefs = moduleSet->getStructDefs();
    std::set<std::string> resolved;
    bool progress = true;

    // Iterative bottom-up: resolve structs whose deps are already resolved
    while (progress)
    {
        progress = false;
        for (auto& pair : structDefs)
        {
            CTSStructDef* def = pair.second;
            if (def->getSVFType())
            {
                resolved.insert(def->getName());
                continue;
            }

            bool canResolve = true;
            std::vector<const SVFType*> fieldTypes;
            const auto& fieldTypeNodes = def->getFieldTypeNodes();
            const auto& fieldDecls = def->getFieldDeclarators();
            CTSSourceFile* file = def->getSourceFile();

            for (size_t i = 0; i < def->getFieldCount(); i++)
            {
                TSNode ftNode = fieldTypeNodes[i];
                // Check for unresolved struct dependency
                if (!ts_node_is_null(ftNode) &&
                    CTSParser::isStructSpecifier(ftNode))
                {
                    std::string depName = CTSParser::getStructName(
                        ftNode, file->getSource());
                    if (!depName.empty() &&
                        resolved.find(depName) == resolved.end())
                    {
                        canResolve = false;
                        break;
                    }
                }
                // Resolve field type (including pointer/array from declarator)
                const SVFType* ft = resolveFullType(
                    ftNode, fieldDecls[i], file);
                fieldTypes.push_back(ft);
            }

            if (canResolve)
            {
                // Compute byte size from field types
                u32_t byteSize = 0;
                for (const auto* ft : fieldTypes)
                    byteSize += ft->getByteSize();

                SVFStructType* svfType = moduleSet->createStructType(
                    def->getName(), fieldTypes, byteSize);
                def->setSVFType(svfType);
                resolved.insert(def->getName());
                progress = true;
            }
        }
    }

    // Fallback: any unresolved (cyclic dependency) structs → use ptr for unknown fields
    for (auto& pair : structDefs)
    {
        CTSStructDef* def = pair.second;
        if (!def->getSVFType())
        {
            std::vector<const SVFType*> fieldTypes;
            const auto& fieldTypeNodes = def->getFieldTypeNodes();
            const auto& fieldDecls = def->getFieldDeclarators();
            CTSSourceFile* file = def->getSourceFile();

            for (size_t i = 0; i < def->getFieldCount(); i++)
            {
                const SVFType* ft = resolveFullType(
                    fieldTypeNodes[i], fieldDecls[i], file);
                fieldTypes.push_back(ft);
            }

            u32_t byteSize = 0;
            for (const auto* ft : fieldTypes)
                byteSize += ft->getByteSize();

            SVFStructType* svfType = moduleSet->createStructType(
                def->getName(), fieldTypes, byteSize);
            def->setSVFType(svfType);
        }
    }
}

StInfo* CTSSVFIRBuilder::collectTypeInfo(const SVFType* type)
{
    auto it = type2StInfo.find(type);
    if (it != type2StInfo.end()) return it->second;

    StInfo* stInfo;
    if (auto* st = SVFUtil::dyn_cast<SVFStructType>(type))
        stInfo = collectStructInfo(st);
    else if (auto* at = SVFUtil::dyn_cast<SVFArrayType>(type))
        stInfo = collectArrayInfo(at);
    else
        stInfo = collectSimpleTypeInfo(type);

    type2StInfo[type] = stInfo;
    pag->addStInfo(stInfo);
    // Set StInfo pointer on the type and register type with PAG
    const_cast<SVFType*>(type)->setTypeInfo(stInfo);
    pag->addTypeInfo(type);
    return stInfo;
}

StInfo* CTSSVFIRBuilder::collectStructInfo(const SVFStructType* st)
{
    StInfo* stInfo = new StInfo(1);
    u32_t numFields = 0;
    u32_t strideOffset = 0;

    for (const SVFType* elemTy : st->getFieldTypes())
    {
        stInfo->addFldWithType(numFields, elemTy, strideOffset);

        if (SVFUtil::isa<SVFStructType, SVFArrayType>(elemTy))
        {
            StInfo* sub = collectTypeInfo(elemTy);
            u32_t nfF = sub->getNumOfFlattenFields();
            u32_t nfE = sub->getNumOfFlattenElements();
            for (u32_t j = 0; j < nfF; j++)
                stInfo->getFlattenFieldTypes().push_back(
                    sub->getFlattenFieldTypes()[j]);
            numFields += nfF;
            strideOffset += nfE;
            for (u32_t j = 0; j < nfE; j++)
                stInfo->getFlattenElementTypes().push_back(
                    sub->getFlattenElementTypes()[j]);
        }
        else
        {
            numFields += 1;
            strideOffset += 1;
            stInfo->getFlattenFieldTypes().push_back(elemTy);
            stInfo->getFlattenElementTypes().push_back(elemTy);
        }
    }

    stInfo->setNumOfFieldsAndElems(numFields, strideOffset);
    return stInfo;
}

StInfo* CTSSVFIRBuilder::collectArrayInfo(const SVFArrayType* at)
{
    u32_t totalElemNum = at->getNumOfElement();
    const SVFType* elemTy = at->getTypeOfElement();

    // Flatten multi-dimensional arrays
    while (auto* innerArr = SVFUtil::dyn_cast<SVFArrayType>(elemTy))
    {
        totalElemNum *= innerArr->getNumOfElement();
        elemTy = innerArr->getTypeOfElement();
    }

    StInfo* stInfo = new StInfo(totalElemNum);

    if (totalElemNum == 0)
    {
        stInfo->addFldWithType(0, elemTy, 0);
        stInfo->setNumOfFieldsAndElems(1, 1);
        stInfo->getFlattenFieldTypes().push_back(elemTy);
        stInfo->getFlattenElementTypes().push_back(elemTy);
        return stInfo;
    }

    StInfo* elemStInfo = collectTypeInfo(elemTy);
    u32_t nfF = elemStInfo->getNumOfFlattenFields();
    u32_t nfE = elemStInfo->getNumOfFlattenElements();

    for (u32_t j = 0; j < nfF; j++)
        stInfo->getFlattenFieldTypes().push_back(
            elemStInfo->getFlattenFieldTypes()[j]);

    u32_t outArrayElemNum = at->getNumOfElement();
    for (u32_t i = 0; i < outArrayElemNum; ++i)
    {
        auto idx = (totalElemNum > 0 && outArrayElemNum > 0)
            ? (i * nfE * totalElemNum) / outArrayElemNum : 0;
        stInfo->addFldWithType(0, elemTy, idx);
    }

    for (u32_t i = 0; i < totalElemNum; ++i)
    {
        for (u32_t j = 0; j < nfE; ++j)
            stInfo->getFlattenElementTypes().push_back(
                elemStInfo->getFlattenElementTypes()[j]);
    }

    stInfo->setNumOfFieldsAndElems(nfF, nfE * totalElemNum);
    return stInfo;
}

StInfo* CTSSVFIRBuilder::collectSimpleTypeInfo(const SVFType* type)
{
    StInfo* stInfo = new StInfo(1);
    stInfo->addFldWithType(0, type, 0);
    stInfo->getFlattenFieldTypes().push_back(type);
    stInfo->getFlattenElementTypes().push_back(type);
    stInfo->setNumOfFieldsAndElems(1, 1);
    return stInfo;
}

void CTSSVFIRBuilder::buildTypeInfo()
{
    // Create StInfo for all builtin types
    for (const auto& name : {"int", "char", "short", "long", "float", "double", "void", "ptr", "i8", "i32", "i64"})
    {
        SVFType* type = moduleSet->getOrCreateType(name);
        if (type && type2StInfo.find(type) == type2StInfo.end())
            collectTypeInfo(type);
    }

    // Create StInfo for struct types (collectTypeInfo is recursive,
    // so nested struct/array StInfo is created automatically)
    for (auto& pair : moduleSet->getStructDefs())
    {
        SVFType* type = pair.second->getSVFType();
        if (type && type2StInfo.find(type) == type2StInfo.end())
            collectTypeInfo(type);
    }

    // Create StInfo for ALL owned types (including array types created during
    // declaration processing, function types, etc.)
    for (SVFType* type : moduleSet->getOwnedTypes())
    {
        if (type && type2StInfo.find(type) == type2StInfo.end())
            collectTypeInfo(type);
    }
}


//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

ICFGNode* CTSSVFIRBuilder::getICFGNode(TSNode stmt, CTSSourceFile* file)
{
    if (!icfgBuilder) return nullptr;
    return icfgBuilder->getStmtICFGNode(stmt, file);
}

NodeID CTSSVFIRBuilder::getFieldGepNode(TSNode fieldExpr, CTSSourceFile* file)
{
    TSNode base = ts_node_child_by_field_name(fieldExpr, "argument", 8);
    TSNode fieldNode = ts_node_child_by_field_name(fieldExpr, "field", 5);
    std::string fieldName = CTSParser::getNodeText(fieldNode, file->getSource());

    // Get base address (lvalue of the struct variable)
    NodeID baseAddr = getExprLValue(base, file);

    // Resolve struct type from base variable
    std::string structName;
    const SVFType* structType = nullptr;
    if (strcmp(ts_node_type(base), "identifier") == 0)
    {
        std::string varName = CTSParser::getNodeText(base, file->getSource());
        auto* varInfo = scopeManager->lookupVar(varName);
        if (varInfo && varInfo->type)
        {
            if (auto* st = SVFUtil::dyn_cast<SVFStructType>(varInfo->type))
            {
                structName = const_cast<SVFStructType*>(st)->getName();
                structType = st;
            }
        }
    }

    // Look up field index
    APOffset fieldIdx = 0;
    CTSStructDef* structDef = moduleSet->getStructDef(structName);
    if (structDef)
    {
        int idx = structDef->getFieldIndex(fieldName);
        if (idx >= 0) fieldIdx = idx;
        if (!structType) structType = structDef->getSVFType();
    }

    // Create GEP: result = GEP(baseAddr, fieldIdx)
    NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
    AccessPath ap(fieldIdx, structType);
    addGepEdge(baseAddr, result, ap);
    return result;
}

NodeID CTSSVFIRBuilder::getArrayGepNode(TSNode subscriptExpr, CTSSourceFile* file)
{
    TSNode array = ts_node_child_by_field_name(subscriptExpr, "argument", 8);
    TSNode index = ts_node_child_by_field_name(subscriptExpr, "index", 5);

    // Get base array address
    NodeID baseAddr = getExprLValue(array, file);

    // Evaluate index expression
    NodeID indexVal = getExprValue(index, file);

    // Determine array type and whether index is constant
    const SVFType* arrayType = nullptr;
    bool constGep = false;
    APOffset constIdx = 0;

    if (strcmp(ts_node_type(index), "number_literal") == 0)
    {
        std::string idxText = CTSParser::getNodeText(index, file->getSource());
        try { constIdx = std::stol(idxText); constGep = true; }
        catch (...) {}
    }

    if (strcmp(ts_node_type(array), "identifier") == 0)
    {
        std::string varName = CTSParser::getNodeText(array, file->getSource());
        auto* varInfo = scopeManager->lookupVar(varName);
        if (varInfo && varInfo->type)
            arrayType = varInfo->type;
    }

    // Create GEP node
    NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
    AccessPath ap(constIdx, arrayType);
    if (!constGep)
    {
        ap.addOffsetVarAndGepTypePair(pag->getGNode(indexVal), arrayType);
    }
    addGepEdge(baseAddr, result, ap, constGep);
    return result;
}

} // namespace SVF
