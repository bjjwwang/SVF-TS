#include "CTS/CTSSVFIRBuilder.h"
#include "CTS/CTSParser.h"
#include "SVFIR/SVFStatements.h"
#include "SVFIR/ObjTypeInfo.h"
#include "Graphs/ICFGNode.h"
#include "Graphs/ICFGEdge.h"
#include "Graphs/BasicBlockG.h"
#include "Graphs/CHG.h"
#include "Graphs/CallGraph.h"
#include "Util/CallGraphBuilder.h"
#include "Util/NodeIDAllocator.h"
#include "Util/SVFLoopAndDomInfo.h"
#include "Util/ExtAPI.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <functional>
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

    // Step 1a: Pre-register known external functions that appear in source code
    // Tree-Sitter doesn't process #include, so functions like memcpy/strcpy
    // may be called without a declaration. We scan all call_expressions and
    // add matching external functions so ICFG builder can create CallICFGNodes.
    registerKnownExternalCalls();

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
    // Set byte size for arrays (needed for buffer overflow detection)
    if (type)
    {
        u32_t byteSize = type->getByteSize();
        if (byteSize > 0)
        {
            ti->setByteSizeOfObj(byteSize);
            ti->setNumOfElements(byteSize);
        }
    }
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
                                       const ICFGNode* icfgNode, u32_t byteSize)
{
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(moduleSet->getPtrType());
    ti->setFlag(ObjTypeInfo::HEAP_OBJ);
    if (byteSize > 0)
    {
        ti->setByteSizeOfObj(byteSize);
        ti->setNumOfElements(byteSize);
    }
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
    CallPE* callPE = pag->addCallPE(src, dst, cs, entry);
    if (callPE)
    {
        callPE->setValue(pag->getGNode(dst));

        // Register on the CallICFGNode's statement list (so AE can see it)
        ICFGNode* callNode = const_cast<ICFGNode*>(static_cast<const ICFGNode*>(cs));
        pag->addToSVFStmtList(callNode, callPE);
        callNode->addSVFStmt(callPE);

        // Register on the CallCFGEdge (matching LLVM frontend behavior)
        CallICFGNode* callICFG = const_cast<CallICFGNode*>(cs);
        FunEntryICFGNode* entryICFG = const_cast<FunEntryICFGNode*>(entry);
        if (ICFGEdge* icfgEdge = pag->getICFG()->hasInterICFGEdge(callICFG, entryICFG, ICFGEdge::CallCF))
            SVFUtil::cast<CallCFGEdge>(icfgEdge)->addCallPE(callPE);
    }
}

void CTSSVFIRBuilder::addRetEdge(NodeID src, NodeID dst, const CallICFGNode* cs,
                                  const FunExitICFGNode* exit)
{
    RetPE* retPE = pag->addRetPE(src, dst, cs, exit);
    if (retPE)
    {
        retPE->setValue(pag->getGNode(dst));

        // Register on the RetICFGNode's statement list (so AE can see it)
        RetICFGNode* retNode = const_cast<RetICFGNode*>(cs->getRetICFGNode());
        if (retNode)
        {
            pag->addToSVFStmtList(retNode, retPE);
            retNode->addSVFStmt(retPE);
        }

        // Register on the RetCFGEdge (matching LLVM frontend behavior)
        FunExitICFGNode* exitICFG = const_cast<FunExitICFGNode*>(exit);
        if (retNode)
        {
            if (ICFGEdge* icfgEdge = pag->getICFG()->hasInterICFGEdge(exitICFG, retNode, ICFGEdge::RetCF))
                SVFUtil::cast<RetCFGEdge>(icfgEdge)->addRetPE(retPE);
        }
    }
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
        if (currentICFGNode)
        {
            pag->addToSVFStmtList(currentICFGNode, edge);
            currentICFGNode->addSVFStmt(edge);
        }
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

        // Register ExtAPI annotations for known external functions
        // (In LLVM frontend this comes from extapi.bc metadata; CTS must do it by name)
        if (!func->hasBody())
        {
            const std::string& fname = func->getName();
            static const std::set<std::string> heapAllocators = {
                "malloc", "calloc", "realloc", "strdup", "strndup",
                "alloca", "valloc", "pvalloc", "memalign", "aligned_alloc",
                "safe_malloc", "safe_calloc", "safe_realloc",
                "xmalloc", "xcalloc", "xrealloc",
                "mmap", "mmap64", "fopen", "fopen64", "fdopen",
                "getcwd", "tmpnam", "tempnam", "opendir", "sbrk"
            };
            if (heapAllocators.count(fname))
            {
                ExtAPI::getExtAPI()->setExtFuncAnnotations(
                    funObj, {"ALLOC_HEAP_RET"});
            }
        }

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

            // Use pointer type for all args so that isPTAEdge() returns true
            // for CallPE edges involving pointer parameters. Without this,
            // Andersen's constraint graph ignores CallPE edges and pointer
            // arguments (like &a) are not propagated to formal parameters.
            NodeID argId = NodeIDAllocator::get()->allocateValueId();
            pag->addArgValNode(argId, i, nullptr, funObj,
                               moduleSet->getPtrType());
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
    // Set currentICFGNode to GlobalICFGNode for global init statements
    currentICFGNode = pag->getICFG()->getGlobalICFGNode();
    for (auto& entry : globalEntries)
    {
        CTSGlobalVar* gvar = entry.first;
        NodeID valId = entry.second;
        TSNode init = gvar->getInitializer();
        if (!ts_node_is_null(init))
        {
            // Array initializer list: {val0, val1, ...} → GEP+Store for each element
            if (strcmp(ts_node_type(init), "initializer_list") == 0)
            {
                uint32_t count = ts_node_named_child_count(init);
                const SVFType* baseType = resolveType(gvar->getTypeNode(), gvar->getSourceFile());
                for (uint32_t idx = 0; idx < count; idx++)
                {
                    NodeID elemVal = getExprValue(ts_node_named_child(init, idx), gvar->getSourceFile());
                    NodeID gepNode = createValNode(moduleSet->getPtrType(), currentICFGNode);
                    AccessPath ap((APOffset)idx, baseType);
                    addGepEdge(valId, gepNode, ap, /*constGep=*/true);
                    addStoreEdge(elemVal, gepNode, currentICFGNode);
                }
            }
            else
            {
                NodeID initVal = getExprValue(init, gvar->getSourceFile());
                if (initVal != blackHoleNode)
                {
                    addStoreEdge(initVal, valId, currentICFGNode);
                }
            }
        }
    }
    currentICFGNode = nullptr;
}

void CTSSVFIRBuilder::processFunction(CTSFunction* func)
{
    currentFunc = func;
    scopeManager->pushScope();
    ssaBuilder->clear();

    const FunObjVar* funObj = func->getFunObjVar();
    CTSSourceFile* file = func->getSourceFile();

    // Set currentICFGNode to FunEntry so parameter AddrStmt/StoreStmt
    // are registered on the entry node (AE needs to see them)
    ICFG* icfg = pag->getICFG();
    FunEntryICFGNode* entryNode = icfg->getFunEntryICFGNode(funObj);
    if (entryNode)
        currentICFGNode = entryNode;

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
    else if (strcmp(type, "labeled_statement") == 0)
    {
        // Process the inner statement (skip the label itself)
        TSNode innerStmt = ts_node_child_by_field_name(stmt, "body", 4);
        if (!ts_node_is_null(innerStmt))
            processStatement(innerStmt, file);
    }
    // goto_statement: no-op in SVFIR (edge already in ICFG)
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
            // If simple stoul failed (e.g. "10+1"), try constant expression eval
            if (arraySize == 0)
            {
                TSNode sizeNode = ts_node_child_by_field_name(innerDecl, "size", 4);
                if (!ts_node_is_null(sizeNode))
                    arraySize = tryEvalConstExpr(sizeNode, file);
            }
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
                // Array initializer list: {val0, val1, ...} → GEP+Store for each element
                if (strcmp(ts_node_type(init), "initializer_list") == 0 &&
                    type && SVFUtil::isa<SVFArrayType>(type))
                {
                    uint32_t count = ts_node_named_child_count(init);
                    for (uint32_t idx = 0; idx < count; idx++)
                    {
                        NodeID elemVal = getExprValue(ts_node_named_child(init, idx), file);
                        NodeID gepNode = createValNode(moduleSet->getPtrType(), currentICFGNode);
                        const SVFType* elemType = SVFUtil::cast<SVFArrayType>(type)->getTypeOfElement();
                        AccessPath ap((APOffset)idx, elemType);
                        addGepEdge(valId, gepNode, ap, /*constGep=*/true);
                        addStoreEdge(elemVal, gepNode, currentICFGNode);
                    }
                }
                // String literal init for char arrays: "ABC" → store each char + '\0'
                else if (strcmp(ts_node_type(init), "string_literal") == 0 &&
                         type && SVFUtil::isa<SVFArrayType>(type))
                {
                    std::string text = CTSParser::getNodeText(init, file->getSource());
                    // Strip quotes
                    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                        text = text.substr(1, text.size() - 2);
                    const SVFType* elemType = SVFUtil::cast<SVFArrayType>(type)->getTypeOfElement();
                    for (u32_t idx = 0; idx <= text.size(); idx++)
                    {
                        char ch = (idx < text.size()) ? text[idx] : '\0';
                        NodeID charVal = createConstIntNode((s64_t)ch, currentICFGNode);
                        NodeID gepNode = createValNode(moduleSet->getPtrType(), currentICFGNode);
                        AccessPath ap((APOffset)idx, elemType);
                        addGepEdge(valId, gepNode, ap, /*constGep=*/true);
                        addStoreEdge(charVal, gepNode, currentICFGNode);
                    }
                }
                else
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

    // Handle malloc/calloc/alloca/realloc as heap/stack allocation
    // Note: tree-sitter doesn't expand macros, so we also match common
    // macro names like ALLOCA, MALLOC, etc.
    {
        static const std::set<std::string> allocNames = {
            "malloc", "calloc", "alloca", "realloc",
            "valloc", "memalign", "aligned_alloc",
            "strdup", "strndup", "mmap",
            "ALLOCA", "MALLOC", "CALLOC", "REALLOC",
            "xmalloc", "xcalloc", "xrealloc",
            "safe_malloc", "safe_calloc", "safe_realloc",
        };
        if (allocNames.count(funcName))
        {
            // Try to extract allocation size from first argument
            u32_t allocSize = 0;
            TSNode args = ts_node_child_by_field_name(call, "arguments", 9);
            if (!ts_node_is_null(args) && ts_node_named_child_count(args) > 0)
            {
                TSNode sizeArg = ts_node_named_child(args, 0);
                allocSize = tryEvalConstExpr(sizeArg, file);
            }
            return createHeapObj(call, file, currentICFGNode, allocSize);
        }
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

    // Look up callee. If not found but the name matches a known external
    // function (memcpy, strcpy, etc.), create a FunObjVar on the fly so
    // that AE's handleExtAPI can recognize and model it.
    CTSFunction* calleeFunc = moduleSet->getFunction(funcName);
    const FunObjVar* calleeFunObj = calleeFunc ? calleeFunc->getFunObjVar() : nullptr;
    if (!calleeFunObj && !funcName.empty())
    {
        calleeFunObj = getOrCreateExtFunObjVar(funcName);
    }

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
            bool isExternal = SVFUtil::isExtCall(calleeFunObj);

            if (!isExternal)
            {
                // Internal function: create CallPE/RetPE for interprocedural analysis
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
                    FunExitICFGNode* exit = icfg->getFunExitICFGNode(calleeFunObj);
                    RetICFGNode* retICFGNode = const_cast<RetICFGNode*>(callICFGNode->getRetICFGNode());

                    // Create result node on the RetICFGNode so that any subsequent
                    // StoreStmt using this result is attached AFTER the RetPE
                    NodeID resultNode = createValNode(moduleSet->getPtrType(),
                        retICFGNode ? static_cast<ICFGNode*>(retICFGNode) : callICFGNode);
                    addRetEdge(pag->getFunRet(calleeFunObj)->getId(), resultNode,
                              callICFGNode, exit);
                    if (retICFGNode)
                        pag->addCallSiteRets(retICFGNode, pag->getGNode(resultNode));

                    currentICFGNode = retICFGNode ? static_cast<ICFGNode*>(retICFGNode) : savedICFGNode;
                    return resultNode;
                }
            }
            else
            {
                // External function: NO RetPE (callee has no body to return from).
                // AE's handleExtAPI sets the return value directly on actualRet.
                RetICFGNode* retICFGNode = const_cast<RetICFGNode*>(callICFGNode->getRetICFGNode());
                ICFGNode* resultICFGNode = retICFGNode ? static_cast<ICFGNode*>(retICFGNode) : callICFGNode;

                // For allocators (malloc, calloc, alloca, etc.), create a heap
                // ObjVar and AddrStmt so that the return value has a proper
                // address in the abstract domain, matching LLVM frontend behavior.
                if (SVFUtil::isHeapAllocExtFunViaRet(calleeFunObj))
                {
                    u32_t allocSz = 0;
                    TSNode callArgs = ts_node_child_by_field_name(call, "arguments", 9);
                    if (!ts_node_is_null(callArgs) && ts_node_named_child_count(callArgs) > 0)
                        allocSz = tryEvalConstExpr(ts_node_named_child(callArgs, 0), file);
                    NodeID heapVal = createHeapObj(call, file, resultICFGNode, allocSz);
                    if (retICFGNode)
                        pag->addCallSiteRets(retICFGNode, pag->getGNode(heapVal));
                    currentICFGNode = retICFGNode ? static_cast<ICFGNode*>(retICFGNode) : savedICFGNode;
                    return heapVal;
                }

                // Non-allocator external: create plain result node
                NodeID resultNode = createValNode(moduleSet->getPtrType(), resultICFGNode);
                if (retICFGNode)
                    pag->addCallSiteRets(retICFGNode, pag->getGNode(resultNode));

                // Switch to retNode for subsequent stores
                currentICFGNode = retICFGNode ? static_cast<ICFGNode*>(retICFGNode) : savedICFGNode;
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
    TSNode cond = ts_node_child_by_field_name(ifStmt, "condition", 9);
    TSNode innerCond = cond;
    if (!ts_node_is_null(cond) &&
        strcmp(ts_node_type(cond), "parenthesized_expression") == 0)
        innerCond = ts_node_named_child(cond, 0);

    // Flatten && / || chain (mirrors ICFG builder)
    std::vector<TSNode> condParts;
    bool isLogicalChain = false;
    std::string logicalOp;
    if (!ts_node_is_null(innerCond) &&
        strcmp(ts_node_type(innerCond), "binary_expression") == 0)
    {
        TSNode opNode = ts_node_child(innerCond, 1);
        logicalOp = CTSParser::getNodeText(opNode, file->getSource());
        if (logicalOp == "&&" || logicalOp == "||")
        {
            isLogicalChain = true;
            std::function<void(TSNode)> flatten = [&](TSNode expr) {
                if (strcmp(ts_node_type(expr), "binary_expression") == 0)
                {
                    TSNode op2 = ts_node_child(expr, 1);
                    if (CTSParser::getNodeText(op2, file->getSource()) == logicalOp)
                    {
                        flatten(ts_node_child_by_field_name(expr, "left", 4));
                        flatten(ts_node_child_by_field_name(expr, "right", 5));
                        return;
                    }
                }
                condParts.push_back(expr);
            };
            flatten(innerCond);
        }
    }

    if (isLogicalChain)
    {
        // Evaluate each sub-condition on its own condNode and set branch conditions.
        // ICFG builder created one condNode per condPart, recorded by sub-expression.
        for (size_t i = 0; i < condParts.size(); i++)
        {
            ICFGNode* partNode = getICFGNode(condParts[i], file);
            if (partNode) currentICFGNode = partNode;
            NodeID partCondVal = getExprValue(condParts[i], file);

            std::cerr << "[SVFIR-&&]   partCondVal=" << partCondVal
                      << " currentICFGNode=" << (currentICFGNode ? (int)currentICFGNode->getId() : -1) << std::endl;
            if (partNode && partCondVal != blackHoleNode)
            {
                const SVFVar* partCondVar = pag->getGNode(partCondVal);
                std::set<const ICFGNode*> dstNodes;
                for (auto it = partNode->OutEdgeBegin(); it != partNode->OutEdgeEnd(); ++it)
                    if (SVFUtil::isa<IntraCFGEdge>(*it))
                        dstNodes.insert((*it)->getDstNode());

                std::cerr << "[SVFIR-&&]   dstNodes=" << dstNodes.size() << std::endl;
                if (dstNodes.size() >= 2)
                {
                    // Find the "true" target
                    ICFGNode* trueTarget = nullptr;
                    if (logicalOp == "&&")
                    {
                        if (i + 1 < condParts.size())
                            trueTarget = getICFGNode(condParts[i + 1], file);
                        else
                        {
                            // Last condNode: true → then-body. Find it.
                            TSNode consequence = ts_node_child_by_field_name(ifStmt, "consequence", 11);
                            if (!ts_node_is_null(consequence))
                            {
                                trueTarget = getICFGNode(consequence, file);
                                if (!trueTarget && strcmp(ts_node_type(consequence), "compound_statement") == 0)
                                {
                                    uint32_t cnt = ts_node_named_child_count(consequence);
                                    for (uint32_t j = 0; j < cnt && !trueTarget; j++)
                                        trueTarget = getICFGNode(ts_node_named_child(consequence, j), file);
                                }
                            }
                        }
                    }
                    else // ||
                    {
                        // true → then-body always
                        TSNode consequence = ts_node_child_by_field_name(ifStmt, "consequence", 11);
                        if (!ts_node_is_null(consequence))
                        {
                            trueTarget = getICFGNode(consequence, file);
                            if (!trueTarget && strcmp(ts_node_type(consequence), "compound_statement") == 0)
                            {
                                uint32_t cnt = ts_node_named_child_count(consequence);
                                for (uint32_t j = 0; j < cnt && !trueTarget; j++)
                                    trueTarget = getICFGNode(ts_node_named_child(consequence, j), file);
                            }
                        }
                    }

                    if (trueTarget)
                    {
                        for (auto it = partNode->OutEdgeBegin(); it != partNode->OutEdgeEnd(); ++it)
                        {
                            if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                            {
                                s32_t cv = (edge->getDstNode() == trueTarget) ? 1 : 0;
                                icfgBuilder->setEdgeCondition(edge, partCondVar, cv);
                            }
                        }
                        BranchStmt::SuccAndCondPairVec succs;
                        for (auto it = partNode->OutEdgeBegin(); it != partNode->OutEdgeEnd(); ++it)
                        {
                            if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                            {
                                s32_t cv = (edge->getDstNode() == trueTarget) ? 1 : 0;
                                succs.push_back(std::make_pair(edge->getDstNode(), cv));
                            }
                        }
                        if (!succs.empty())
                            addBranchEdge(partCondVal, partCondVal, succs);
                    }
                }
            }
        }
    }
    else
    {
        // Single condition (original path)
        NodeID condVal = blackHoleNode;
        if (!ts_node_is_null(innerCond))
            condVal = getExprValue(innerCond, file);

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
    } // end else (single condition)

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
    // while: getICFGNode(whileStmt) = condNode (recorded in ICFG builder)
    // condNode has edges: condNode → bodyFirst (true), condNode → afterNode (false)
    ICFGNode* condICFGNode = getICFGNode(whileStmt, file);

    // Evaluate condition ON the condNode (so CmpStmt attaches to the right node)
    TSNode cond = ts_node_child_by_field_name(whileStmt, "condition", 9);
    NodeID condVal = blackHoleNode;
    if (!ts_node_is_null(cond))
    {
        if (condICFGNode) currentICFGNode = condICFGNode;
        condVal = getExprValue(cond, file);
    }

    // Set branch conditions on condNode's outgoing edges
    if (condICFGNode && condVal != blackHoleNode)
    {
        const SVFVar* condVar = pag->getGNode(condVal);

        std::set<const ICFGNode*> dstNodes;
        for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
            if (SVFUtil::isa<IntraCFGEdge>(*it))
                dstNodes.insert((*it)->getDstNode());

        if (dstNodes.size() >= 2)
        {
            TSNode bodyNode = CTSParser::getLoopBody(whileStmt);
            ICFGNode* bodyICFGNode = nullptr;
            if (!ts_node_is_null(bodyNode))
            {
                bodyICFGNode = getICFGNode(bodyNode, file);
                if (!bodyICFGNode && strcmp(ts_node_type(bodyNode), "compound_statement") == 0)
                {
                    uint32_t cnt = ts_node_named_child_count(bodyNode);
                    for (uint32_t i = 0; i < cnt && !bodyICFGNode; i++)
                        bodyICFGNode = getICFGNode(ts_node_named_child(bodyNode, i), file);
                }
            }

            if (bodyICFGNode)
            {
                BranchStmt::SuccAndCondPairVec succs;
                for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
                {
                    if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                    {
                        s32_t cv = (edge->getDstNode() == bodyICFGNode) ? 1 : 0;
                        icfgBuilder->setEdgeCondition(edge, condVar, cv);
                        succs.push_back(std::make_pair(edge->getDstNode(), cv));
                    }
                }
                if (!succs.empty())
                    addBranchEdge(condVal, condVal, succs);
            }
        }
    }

    TSNode body = CTSParser::getLoopBody(whileStmt);
    if (!ts_node_is_null(body))
    {
        processStatement(body, file);
    }
}

void CTSSVFIRBuilder::processForStatement(TSNode forStmt, CTSSourceFile* file)
{
    // for: getICFGNode(forStmt) = initNode (recorded in ICFG builder)
    // ICFG structure: initNode → condNode → bodyFirst / afterNode
    //                                   updateNode → condNode (back-edge)
    ICFGNode* initICFGNode = getICFGNode(forStmt, file);

    // Process init on initNode
    TSNode init = ts_node_child_by_field_name(forStmt, "initializer", 11);
    if (!ts_node_is_null(init))
    {
        processStatement(init, file);
    }

    // Find condNode: it's the sole successor of initNode
    ICFGNode* condICFGNode = nullptr;
    if (initICFGNode)
    {
        for (auto it = initICFGNode->OutEdgeBegin(); it != initICFGNode->OutEdgeEnd(); ++it)
        {
            if (SVFUtil::isa<IntraCFGEdge>(*it))
            {
                condICFGNode = (*it)->getDstNode();
                break;
            }
        }
    }

    // Evaluate condition ON the condNode
    TSNode cond = ts_node_child_by_field_name(forStmt, "condition", 9);
    NodeID condVal = blackHoleNode;
    if (!ts_node_is_null(cond))
    {
        if (condICFGNode) currentICFGNode = condICFGNode;
        condVal = getExprValue(cond, file);
    }

    // Set branch conditions on condNode's edges
    if (condICFGNode && condVal != blackHoleNode)
    {
        const SVFVar* condVar = pag->getGNode(condVal);

        std::set<const ICFGNode*> dstNodes;
        for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
            if (SVFUtil::isa<IntraCFGEdge>(*it))
                dstNodes.insert((*it)->getDstNode());

        if (dstNodes.size() >= 2)
        {
            // Find loop body's first ICFG node
            TSNode bodyNode = CTSParser::getLoopBody(forStmt);
            ICFGNode* bodyICFGNode = nullptr;
            if (!ts_node_is_null(bodyNode))
            {
                bodyICFGNode = getICFGNode(bodyNode, file);
                if (!bodyICFGNode && strcmp(ts_node_type(bodyNode), "compound_statement") == 0)
                {
                    uint32_t cnt = ts_node_named_child_count(bodyNode);
                    for (uint32_t i = 0; i < cnt && !bodyICFGNode; i++)
                        bodyICFGNode = getICFGNode(ts_node_named_child(bodyNode, i), file);
                }
            }

            if (bodyICFGNode)
            {
                BranchStmt::SuccAndCondPairVec succs;
                for (auto it = condICFGNode->OutEdgeBegin(); it != condICFGNode->OutEdgeEnd(); ++it)
                {
                    if (IntraCFGEdge* edge = SVFUtil::dyn_cast<IntraCFGEdge>(*it))
                    {
                        s32_t cv = (edge->getDstNode() == bodyICFGNode) ? 1 : 0;
                        icfgBuilder->setEdgeCondition(edge, condVar, cv);
                        succs.push_back(std::make_pair(edge->getDstNode(), cv));
                    }
                }
                if (!succs.empty())
                    addBranchEdge(condVal, condVal, succs);
            }
        }
    }

    // Find updateNode: condNode's successor that is NOT bodyFirst and NOT afterNode
    // Actually, updateNode is the node that has a back-edge to condNode
    // Process update expression on the updateNode
    TSNode update = ts_node_child_by_field_name(forStmt, "update", 6);
    if (!ts_node_is_null(update))
    {
        // Find updateNode: it's the predecessor of condNode that is NOT initNode
        if (condICFGNode)
        {
            for (auto it = condICFGNode->InEdgeBegin(); it != condICFGNode->InEdgeEnd(); ++it)
            {
                ICFGNode* pred = (*it)->getSrcNode();
                if (pred != initICFGNode && pred != condICFGNode)
                {
                    currentICFGNode = pred; // updateNode
                    break;
                }
            }
        }
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
    // Get the condNode that ICFG builder created for this switch
    ICFGNode* condNode = getICFGNode(switchStmt, file);

    // Evaluate the switch condition on condNode
    TSNode cond = ts_node_child_by_field_name(switchStmt, "condition", 9);
    NodeID condVar = blackHoleNode;
    if (!ts_node_is_null(cond))
    {
        ICFGNode* savedNode = currentICFGNode;
        if (condNode) currentICFGNode = condNode;
        condVar = getExprValue(cond, file);
        currentICFGNode = savedNode;
    }

    // Collect case nodes and their values for BranchStmt
    TSNode body = ts_node_child_by_field_name(switchStmt, "body", 4);
    BranchStmt::SuccAndCondPairVec successors;

    if (!ts_node_is_null(body))
    {
        uint32_t count = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_named_child(body, i);
            const char* childType = ts_node_type(child);
            if (strcmp(childType, "case_statement") == 0)
            {
                // Get case value: first child is the value expression
                // case_statement has "value" field for case X:, null for default:
                TSNode caseValue = ts_node_child_by_field_name(child, "value", 5);
                s64_t val = -1; // default case
                if (!ts_node_is_null(caseValue))
                {
                    std::string valText = CTSParser::getNodeText(caseValue, file->getSource());
                    // Handle integer literals and char literals
                    if (valText.size() >= 2 && valText[0] == '\'')
                        val = (s64_t)valText[1];
                    else
                    {
                        try { val = std::stol(valText); }
                        catch (...) { val = -1; }
                    }
                }

                // Find the ICFG node for this case's first statement
                ICFGNode* caseICFGNode = nullptr;
                uint32_t caseCount = ts_node_named_child_count(child);
                for (uint32_t j = 0; j < caseCount; j++)
                {
                    TSNode caseChild = ts_node_named_child(child, j);
                    caseICFGNode = getICFGNode(caseChild, file);
                    if (caseICFGNode) break;
                }
                if (caseICFGNode)
                    successors.push_back(std::make_pair(caseICFGNode, val));

                // Process statements inside case
                for (uint32_t j = 0; j < caseCount; j++)
                {
                    TSNode caseChild = ts_node_named_child(child, j);
                    const char* ccType = ts_node_type(caseChild);
                    if (strcmp(ccType, "break_statement") == 0) continue;
                    if (strcmp(ccType, "expression_statement") == 0 ||
                        strcmp(ccType, "declaration") == 0 ||
                        strcmp(ccType, "return_statement") == 0 ||
                        strcmp(ccType, "compound_statement") == 0 ||
                        strcmp(ccType, "if_statement") == 0)
                    {
                        processStatement(caseChild, file);
                    }
                }
            }
        }
    }

    // Create BranchStmt on condNode (for AE's isSwitchBranchFeasible)
    if (condNode && condVar != blackHoleNode && !successors.empty())
    {
        ICFGNode* savedNode = currentICFGNode;
        currentICFGNode = condNode;
        addBranchEdge(condVar, condVar, successors);
        currentICFGNode = savedNode;
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
            // Array type: array name decays to pointer (address of first element).
            // Return the address directly — no load. Same as &arr[0].
            if (varInfo->type && SVFUtil::isa<SVFArrayType>(varInfo->type))
                return varInfo->valNode;

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
        // Check for && / || FIRST — handle before creating any nodes
        TSNode opNode = ts_node_child(expr, 1);
        std::string op = CTSParser::getNodeText(opNode, file->getSource());
        if (op == "&&")
        {
            // Short-circuit: evaluate both sides for side effects,
            // return rhs CmpStmt result as condVar for branch narrowing
            TSNode left = ts_node_child_by_field_name(expr, "left", 4);
            TSNode right = ts_node_child_by_field_name(expr, "right", 5);
            getExprValue(left, file);   // side effects only
            return getExprValue(right, file);  // condVar = rhs
        }
        if (op == "||")
        {
            TSNode left = ts_node_child_by_field_name(expr, "left", 4);
            TSNode right = ts_node_child_by_field_name(expr, "right", 5);
            NodeID lhsVal = getExprValue(left, file);
            getExprValue(right, file);  // side effects only
            return lhsVal;  // condVar = lhs
        }

        TSNode left = ts_node_child_by_field_name(expr, "left", 4);
        TSNode right = ts_node_child_by_field_name(expr, "right", 5);
        NodeID lhsVal = getExprValue(left, file);
        NodeID rhsVal = getExprValue(right, file);
        NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);

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
        // && and || handled above (early return before node creation)
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

    // Sizeof expression: sizeof(type) or sizeof(expr)
    if (strcmp(type, "sizeof_expression") == 0)
    {
        // Try to resolve the type/expression argument
        TSNode arg = ts_node_named_child(expr, 0);
        if (!ts_node_is_null(arg))
        {
            const char* argType = ts_node_type(arg);
            // sizeof(type_descriptor) — e.g. sizeof(int), sizeof(char)
            if (strcmp(argType, "type_descriptor") == 0 ||
                strcmp(argType, "parenthesized_expression") == 0)
            {
                std::string argText = CTSParser::getNodeText(arg, file->getSource());
                // Remove parens
                if (argText.front() == '(') argText = argText.substr(1);
                if (argText.back() == ')') argText.pop_back();
                // Strip whitespace
                while (!argText.empty() && argText.front() == ' ') argText.erase(0, 1);
                while (!argText.empty() && argText.back() == ' ') argText.pop_back();

                if (argText == "char" || argText == "unsigned char" || argText == "signed char" ||
                    argText == "int8_t" || argText == "uint8_t")
                    return createConstIntNode(1, currentICFGNode);
                if (argText == "short" || argText == "unsigned short" ||
                    argText == "int16_t" || argText == "uint16_t" || argText == "wchar_t")
                    return createConstIntNode(2, currentICFGNode);
                if (argText == "int" || argText == "unsigned int" || argText == "unsigned" ||
                    argText == "int32_t" || argText == "uint32_t" || argText == "float")
                    return createConstIntNode(4, currentICFGNode);
                if (argText == "long" || argText == "unsigned long" || argText == "size_t" ||
                    argText == "long long" || argText == "unsigned long long" ||
                    argText == "int64_t" || argText == "uint64_t" || argText == "double" ||
                    argText == "long int" || argText == "unsigned long int")
                    return createConstIntNode(8, currentICFGNode);
                // Pointer types
                if (argText.find('*') != std::string::npos)
                    return createConstIntNode(8, currentICFGNode);
                // Struct: look up in struct defs
                std::string structName = argText;
                if (structName.find("struct ") == 0)
                    structName = structName.substr(7);
                CTSStructDef* sd = moduleSet->getStructDef(structName);
                if (sd && sd->getSVFType())
                    return createConstIntNode(
                        sd->getSVFType()->getByteSize(), currentICFGNode);
            }
        }
        // Default: 8 bytes (pointer size on 64-bit)
        return createConstIntNode(8, currentICFGNode);
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

u32_t CTSSVFIRBuilder::tryEvalConstExpr(TSNode expr, CTSSourceFile* file)
{
    if (ts_node_is_null(expr)) return 0;
    const char* type = ts_node_type(expr);

    if (strcmp(type, "number_literal") == 0)
    {
        std::string text = CTSParser::getNodeText(expr, file->getSource());
        try { return (u32_t)std::stoul(text); }
        catch (...) { return 0; }
    }

    if (strcmp(type, "sizeof_expression") == 0)
    {
        TSNode arg = ts_node_named_child(expr, 0);
        if (!ts_node_is_null(arg))
        {
            std::string argText = CTSParser::getNodeText(arg, file->getSource());
            if (argText.front() == '(') argText = argText.substr(1);
            if (argText.back() == ')') argText.pop_back();
            while (!argText.empty() && argText.front() == ' ') argText.erase(0, 1);
            while (!argText.empty() && argText.back() == ' ') argText.pop_back();

            if (argText == "char" || argText == "unsigned char") return 1;
            if (argText == "short" || argText == "unsigned short") return 2;
            if (argText == "int" || argText == "unsigned int" || argText == "unsigned" || argText == "float") return 4;
            if (argText == "long" || argText == "unsigned long" || argText == "double" ||
                argText == "long long" || argText == "int64_t" || argText == "uint64_t" ||
                argText == "size_t") return 8;
            if (argText.find('*') != std::string::npos) return 8;
            // Struct lookup
            std::string sn = argText;
            if (sn.find("struct ") == 0) sn = sn.substr(7);
            CTSStructDef* sd = moduleSet->getStructDef(sn);
            if (sd && sd->getSVFType()) return sd->getSVFType()->getByteSize();
        }
        return 8; // default pointer size
    }

    if (strcmp(type, "parenthesized_expression") == 0)
    {
        TSNode inner = ts_node_named_child(expr, 0);
        return tryEvalConstExpr(inner, file);
    }

    if (strcmp(type, "binary_expression") == 0)
    {
        TSNode left = ts_node_child_by_field_name(expr, "left", 4);
        TSNode right = ts_node_child_by_field_name(expr, "right", 5);
        TSNode op = ts_node_child_by_field_name(expr, "operator", 8);
        u32_t lv = tryEvalConstExpr(left, file);
        u32_t rv = tryEvalConstExpr(right, file);
        if (!ts_node_is_null(op))
        {
            std::string opText = CTSParser::getNodeText(op, file->getSource());
            if (opText == "*") return lv * rv;
            if (opText == "+") return lv + rv;
            if (opText == "-") return lv > rv ? lv - rv : 0;
            if (opText == "/") return rv > 0 ? lv / rv : 0;
        }
        return 0;
    }

    // Cast expression: (type)expr
    if (strcmp(type, "cast_expression") == 0)
    {
        // The value is the last named child
        u32_t count = ts_node_named_child_count(expr);
        if (count > 0)
            return tryEvalConstExpr(ts_node_named_child(expr, count - 1), file);
    }

    return 0;
}

void CTSSVFIRBuilder::registerKnownExternalCalls()
{
    // Known external function names (C source level)
    static const std::set<std::string> knownExtNames = {
        "memcpy", "memmove", "memset", "memcmp",
        "strcpy", "strncpy", "strcat", "strncat", "strcmp", "strncmp", "strlen",
        "stpcpy", "wcscpy", "wcscat", "wcsncat",
        "sprintf", "snprintf", "printf", "fprintf", "scanf", "fscanf", "sscanf",
        "puts", "gets", "fgets", "fputs", "fread", "fwrite",
        "malloc", "calloc", "realloc", "free", "alloca",
        "valloc", "memalign", "aligned_alloc", "strdup", "strndup",
        "mmap", "fopen", "fopen64", "fdopen",
        "atoi", "atol", "atof", "strtol", "strtoul",
        "abs", "exit", "abort", "rand", "srand", "time", "clock", "sleep",
        "isdigit", "isalpha", "toupper", "tolower",
        "svf_assert", "svf_print",
        // Buffer overflow checker stub functions
        "UNSAFE_BUFACCESS", "SAFE_BUFACCESS",
        // Common IO/print stubs from test harness
        "printLine", "printWLine", "printIntLine", "printShortLine",
        "printFloatLine", "printLongLine", "printLongLongLine",
        "printUnsignedLine", "printHexCharLine", "printSizeTLine",
    };

    // Walk all source files' ASTs looking for call_expression with known names
    std::function<void(TSNode, CTSSourceFile*)> scan = [&](TSNode node, CTSSourceFile* file) {
        if (ts_node_is_null(node)) return;
        if (strcmp(ts_node_type(node), "call_expression") == 0)
        {
            TSNode funcNode = ts_node_child_by_field_name(node, "function", 8);
            if (!ts_node_is_null(funcNode) && strcmp(ts_node_type(funcNode), "identifier") == 0)
            {
                std::string name = CTSParser::getNodeText(funcNode, file->getSource());
                if (knownExtNames.count(name) && !moduleSet->getFunction(name))
                {
                    moduleSet->addExternalFunction(name);
                }
            }
        }
        for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            scan(ts_node_child(node, i), file);
    };

    for (auto* file : moduleSet->getSourceFiles())
    {
        scan(file->getRootNode(), file);
    }
}

const FunObjVar* CTSSVFIRBuilder::getOrCreateExtFunObjVar(const std::string& funcName)
{
    // Return cached version if already created
    auto it = extFunCache.find(funcName);
    if (it != extFunCache.end())
        return it->second;

    // Known external function annotations (C source-level names)
    static const std::map<std::string, std::vector<std::string>> knownExtFuncs = {
        // Memory copy
        {"memcpy",       {"MEMCPY"}},
        {"memmove",      {"MEMCPY"}},
        {"strncpy",      {"MEMCPY"}},
        {"memccpy",      {"MEMCPY"}},
        {"bcopy",        {"MEMCPY"}},
        // String copy
        {"strcpy",       {"STRCPY"}},
        {"stpcpy",       {"STRCPY"}},
        {"wcscpy",       {"STRCPY"}},
        // String concat
        {"strcat",       {"STRCAT"}},
        {"strncat",      {"STRCAT"}},
        {"wcscat",       {"STRCAT"}},
        {"wcsncat",      {"STRCAT"}},
        // Memory set
        {"memset",       {"MEMSET"}},
        {"wmemset",      {"MEMSET"}},
        // Heap alloc (via ret)
        {"malloc",       {"ALLOC_HEAP_RET"}},
        {"calloc",       {"ALLOC_HEAP_RET"}},
        {"realloc",      {"ALLOC_HEAP_RET"}},
        {"alloca",       {"ALLOC_HEAP_RET"}},
        {"strdup",       {"ALLOC_HEAP_RET"}},
        {"strndup",      {"ALLOC_HEAP_RET"}},
        {"valloc",       {"ALLOC_HEAP_RET"}},
        {"memalign",     {"ALLOC_HEAP_RET"}},
        {"aligned_alloc",{"ALLOC_HEAP_RET"}},
        {"mmap",         {"ALLOC_HEAP_RET"}},
        {"fopen",        {"ALLOC_HEAP_RET"}},
        // Common I/O / misc (no special annotation, just need FunObjVar for isExtCall)
        {"printf",       {}},
        {"fprintf",      {}},
        {"sprintf",      {}},
        {"snprintf",     {}},
        {"scanf",        {}},
        {"fscanf",       {}},
        {"sscanf",       {}},
        {"puts",         {}},
        {"gets",         {}},
        {"fgets",        {}},
        {"fputs",        {}},
        {"fread",        {}},
        {"fwrite",       {}},
        {"strlen",       {}},
        {"strcmp",       {}},
        {"strncmp",      {}},
        {"atoi",         {}},
        {"atol",         {}},
        {"atof",         {}},
        {"strtol",       {}},
        {"strtoul",      {}},
        {"abs",          {}},
        {"exit",         {}},
        {"abort",        {}},
        {"free",         {}},
        {"rand",         {}},
        {"srand",        {}},
        {"time",         {}},
        {"clock",        {}},
        {"sleep",        {}},
        {"isdigit",      {}},
        {"isalpha",      {}},
        {"toupper",      {}},
        {"tolower",      {}},
    };

    auto knownIt = knownExtFuncs.find(funcName);
    if (knownIt == knownExtFuncs.end())
    {
        extFunCache[funcName] = nullptr;
        return nullptr;
    }

    // Create a minimal FunObjVar for this external function
    NodeID objId = NodeIDAllocator::get()->allocateObjectId();
    ObjTypeInfo* ti = pag->createObjTypeInfo(moduleSet->getPtrType());
    ti->setFlag(ObjTypeInfo::FUNCTION_OBJ);
    pag->idToObjTypeInfoMap()[objId] = ti;
    pag->addFunObjNode(objId, ti, nullptr);

    FunObjVar* funObj = SVFUtil::cast<FunObjVar>(pag->getGNode(objId));
    funObj->setName(funcName);

    // Create minimal BBGraph
    BasicBlockGraph* bbGraph = new BasicBlockGraph();
    auto* entryBB = new SVFBasicBlock(1, funObj);
    entryBB->setName(funcName + ".entry");
    bbGraph->addBasicBlock(entryBB);
    auto* exitBB = new SVFBasicBlock(2, funObj);
    exitBB->setName(funcName + ".exit");
    bbGraph->addBasicBlock(exitBB);

    // Create function type
    std::vector<const SVFType*> paramTypes;
    auto* svfFuncType = new SVFFunctionType(
        moduleSet->getTypeIdCounter(), moduleSet->getPtrType(), paramTypes, true);
    moduleSet->addOwnedType(svfFuncType);

    SVFLoopAndDomInfo* loopDom = new SVFLoopAndDomInfo();
    std::vector<const ArgValVar*> emptyArgs;
    funObj->initFunObjVar(
        true,   // isDecl (external)
        false, false, false, false, true,
        svfFuncType, loopDom, funObj, bbGraph, emptyArgs, exitBB);

    // Note: ICFG entry/exit nodes are created by CTSICFGBuilder if the
    // function was pre-registered via registerKnownExternalCalls().
    // For functions created after ICFG build, they won't have ICFG nodes,
    // but that's OK since external calls use IntraCFGEdge (no entry/exit needed).

    // Create FunValVar
    NodeID funValId = NodeIDAllocator::get()->allocateValueId();
    pag->addFunValNode(funValId, nullptr, funObj, moduleSet->getPtrType());

    // Register ExtAPI annotations
    if (!knownIt->second.empty())
    {
        ExtAPI::getExtAPI()->setExtFuncAnnotations(funObj, knownIt->second);
    }

    extFunCache[funcName] = funObj;
    return funObj;
}

NodeID CTSSVFIRBuilder::getFieldGepNode(TSNode fieldExpr, CTSSourceFile* file)
{
    TSNode base = ts_node_child_by_field_name(fieldExpr, "argument", 8);
    TSNode fieldNode = ts_node_child_by_field_name(fieldExpr, "field", 5);
    std::string fieldName = CTSParser::getNodeText(fieldNode, file->getSource());

    // Detect "." vs "->" operator:
    //   s.field  → GEP on lvalue of s (struct address)
    //   p->field → GEP on rvalue of p (load the pointer first)
    bool isArrow = false;
    for (u32_t i = 0; i < ts_node_child_count(fieldExpr); i++)
    {
        TSNode child = ts_node_child(fieldExpr, i);
        if (strcmp(ts_node_type(child), "->") == 0)
        {
            isArrow = true;
            break;
        }
    }

    NodeID gepBase;
    if (isArrow)
        gepBase = getExprValue(base, file);  // load pointer first
    else
        gepBase = getExprLValue(base, file);  // struct address

    // Resolve struct type from base variable
    std::string structName;
    const SVFType* structType = nullptr;
    // Walk through the base to find the root identifier and its type
    TSNode cur = base;
    // For chains like p->a.b, skip field_expression layers
    while (strcmp(ts_node_type(cur), "field_expression") == 0)
        cur = ts_node_child_by_field_name(cur, "argument", 8);
    if (strcmp(ts_node_type(cur), "identifier") == 0)
    {
        std::string varName = CTSParser::getNodeText(cur, file->getSource());
        auto* varInfo = scopeManager->lookupVar(varName);
        if (varInfo && varInfo->type)
        {
            const SVFType* baseTy = varInfo->type;
            // For pointer types (p->field), the struct type is what the pointer points to
            // We don't have explicit pointee info, so look up struct by name
            if (auto* st = SVFUtil::dyn_cast<SVFStructType>(baseTy))
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

    // Create GEP: result = GEP(gepBase, fieldIdx)
    NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
    AccessPath ap(fieldIdx, structType);
    addGepEdge(gepBase, result, ap);
    return result;
}

NodeID CTSSVFIRBuilder::getArrayGepNode(TSNode subscriptExpr, CTSSourceFile* file)
{
    TSNode array = ts_node_child_by_field_name(subscriptExpr, "argument", 8);
    TSNode index = ts_node_child_by_field_name(subscriptExpr, "index", 5);

    // Evaluate index expression
    NodeID indexVal = getExprValue(index, file);

    // Determine index constant value
    bool constGep = false;
    APOffset constIdx = 0;
    if (strcmp(ts_node_type(index), "number_literal") == 0)
    {
        std::string idxText = CTSParser::getNodeText(index, file->getSource());
        try { constIdx = std::stol(idxText); constGep = true; }
        catch (...) {}
    }

    // Resolve element type and decide whether to load the base pointer.
    //
    // Only the FIRST subscript on a pointer-typed variable needs a load:
    //   int arr[2]:    arr[i]   → GEP on lvalue (array addr IS the ptr)
    //   int* ptr:      ptr[i]   → load ptr value first, then GEP
    //   int a[3][3]:   a[i][j]  → outer GEP on lvalue, inner GEP on outer result (no load)
    //
    // When 'array' is itself a subscript_expression, the recursive getExprLValue
    // already returns a GEP result pointer — no extra load needed.
    const SVFType* elementType = nullptr;
    bool needLoad = false;
    {
        // Only check if the immediate base is an identifier (first subscript level)
        if (strcmp(ts_node_type(array), "identifier") == 0)
        {
            std::string varName = CTSParser::getNodeText(array, file->getSource());
            auto* varInfo = scopeManager->lookupVar(varName);
            if (varInfo && varInfo->type)
            {
                if (auto* arrTy = SVFUtil::dyn_cast<SVFArrayType>(varInfo->type))
                {
                    // Local array: GEP directly on lvalue, element type from array
                    elementType = arrTy->getTypeOfElement();
                }
                else
                {
                    // Pointer variable or base type (e.g. int for "int* arr" param)
                    // Need to load the pointer value first, then GEP
                    elementType = varInfo->type;
                    needLoad = true;
                }
            }
        }
        else
        {
            // Nested subscript or other expression — the base is already a pointer
            // from a previous GEP. Resolve element type by walking to root.
            TSNode cur = array;
            int depth = 0;
            while (strcmp(ts_node_type(cur), "subscript_expression") == 0)
            {
                cur = ts_node_child_by_field_name(cur, "argument", 8);
                depth++;
            }
            if (strcmp(ts_node_type(cur), "identifier") == 0)
            {
                std::string varName = CTSParser::getNodeText(cur, file->getSource());
                auto* varInfo = scopeManager->lookupVar(varName);
                if (varInfo && varInfo->type)
                {
                    const SVFType* ty = varInfo->type;
                    for (int i = 0; i < depth; i++)
                    {
                        if (auto* arrTy = SVFUtil::dyn_cast<SVFArrayType>(ty))
                            ty = arrTy->getTypeOfElement();
                        else
                            break;
                    }
                    elementType = ty;
                }
            }
        }
    }

    // Get GEP base
    NodeID gepBase;
    if (needLoad)
        gepBase = getExprValue(array, file);  // load pointer, then GEP
    else
        gepBase = getExprLValue(array, file);  // array or nested: GEP on lvalue

    // Create GEP node
    NodeID result = createValNode(moduleSet->getPtrType(), currentICFGNode);
    AccessPath ap(constIdx, elementType);
    if (!constGep)
    {
        // For variant GEP, pass pointer type as the idx-type pair (matching LLVM:
        // getelementptr i32, ptr %base, i64 %idx → gepTy is ptr for the index).
        // This ensures getElementIndex uses elemNum*idx instead of flattenedElemIdx.
        ap.addOffsetVarAndGepTypePair(pag->getGNode(indexVal), moduleSet->getPtrType());
    }
    addGepEdge(gepBase, result, ap, constGep);
    return result;
}

} // namespace SVF
