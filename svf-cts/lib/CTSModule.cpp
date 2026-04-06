#include "CTS/CTSModule.h"
#include "CTS/CTSParser.h"
#include "SVFIR/SVFIR.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <queue>

namespace fs = std::filesystem;

namespace SVF
{

//===----------------------------------------------------------------------===//
// CTSSourceFile
//===----------------------------------------------------------------------===//

CTSSourceFile::CTSSourceFile(const std::string& path)
    : filePath(path), tree(nullptr), parser(std::make_unique<CTSParser>())
{
}

CTSSourceFile::~CTSSourceFile()
{
    if (tree)
    {
        ts_tree_delete(tree);
    }
}

bool CTSSourceFile::parse()
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        std::cerr << "Error: Cannot open file: " << filePath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    source = buffer.str();

    tree = parser->parse(source);
    return tree != nullptr;
}

TSNode CTSSourceFile::getRootNode() const
{
    if (tree)
    {
        return ts_tree_root_node(tree);
    }
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    return nullNode;
}

std::string CTSSourceFile::getNodeText(TSNode node) const
{
    return CTSParser::getNodeText(node, source);
}

//===----------------------------------------------------------------------===//
// CTSFunction
//===----------------------------------------------------------------------===//

static TSNode makeNullNode()
{
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    return nullNode;
}

CTSFunction::CTSFunction(const std::string& n, TSNode nd, CTSSourceFile* f)
    : name(n), node(nd), sourceFile(f), hasDefinition(false),
      funObjVar(nullptr)
{
    body = makeNullNode();
}

//===----------------------------------------------------------------------===//
// CTSGlobalVar
//===----------------------------------------------------------------------===//

CTSGlobalVar::CTSGlobalVar(const std::string& n, TSNode nd, CTSSourceFile* f)
    : name(n), node(nd), sourceFile(f)
{
    typeNode = makeNullNode();
    initializer = makeNullNode();
}

//===----------------------------------------------------------------------===//
// CTSStructDef
//===----------------------------------------------------------------------===//

CTSStructDef::CTSStructDef(const std::string& n, TSNode nd, CTSSourceFile* f)
    : name(n), node(nd), sourceFile(f), svfType(nullptr)
{
}

void CTSStructDef::addField(const std::string& fieldName, TSNode typeNode, TSNode declarator)
{
    fieldNames.push_back(fieldName);
    fieldTypes.push_back(typeNode);
    fieldDeclarators.push_back(declarator);
}

int CTSStructDef::getFieldIndex(const std::string& fieldName) const
{
    for (size_t i = 0; i < fieldNames.size(); i++)
    {
        if (fieldNames[i] == fieldName)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

//===----------------------------------------------------------------------===//
// CTSModuleSet - Singleton
//===----------------------------------------------------------------------===//

CTSModuleSet* CTSModuleSet::instance = nullptr;

CTSModuleSet::CTSModuleSet()
    : typeIdCounter(0)
{
    initBuiltinTypes();
}

CTSModuleSet::~CTSModuleSet()
{
    for (auto* file : sourceFiles)
        delete file;
    for (auto& pair : functionMap)
        delete pair.second;
    for (auto& pair : globalVarMap)
        delete pair.second;
    for (auto& pair : structDefMap)
        delete pair.second;
    // Don't delete types here — types registered with PAG via addTypeInfo()
    // are owned by IRGraph and deleted in IRGraph::destorySymTable().
    // Types NOT registered with PAG (created before pag exists or never
    // run through collectTypeInfo) would leak, but this is acceptable
    // since they're small and the process is about to exit anyway.
    // Deleting them here would cause double-free with PAG's cleanup.
}

CTSModuleSet* CTSModuleSet::getModuleSet()
{
    if (instance == nullptr)
    {
        instance = new CTSModuleSet();
    }
    return instance;
}

void CTSModuleSet::releaseModuleSet()
{
    delete instance;
    instance = nullptr;
}

void CTSModuleSet::buildFromFiles(const std::vector<std::string>& files)
{
    // Phase 1: Discover all files (including transitively #included headers)
    std::queue<std::string> worklist;
    std::vector<std::string> allFiles;
    std::map<std::string, std::vector<std::string>> deps;
    std::map<std::string, CTSSourceFile*> fileMap;

    for (const auto& f : files)
    {
        std::string absPath;
        try { absPath = fs::canonical(f).string(); }
        catch (...) { absPath = f; }

        if (parsedFiles.find(absPath) == parsedFiles.end())
        {
            worklist.push(absPath);
            parsedFiles.insert(absPath);
            allFiles.push_back(absPath);
        }
    }

    while (!worklist.empty())
    {
        std::string path = worklist.front();
        worklist.pop();

        auto* sf = new CTSSourceFile(path);
        if (!sf->parse())
        {
            std::cerr << "Warning: Failed to parse file: " << path << std::endl;
            delete sf;
            continue;
        }
        fileMap[path] = sf;

        // Extract #include "..." directives and discover dependencies
        auto includes = extractIncludes(sf);
        for (const auto& inc : includes)
        {
            std::string resolved = resolveInclude(inc, path);
            if (!resolved.empty())
            {
                deps[path].push_back(resolved);
                if (parsedFiles.find(resolved) == parsedFiles.end())
                {
                    parsedFiles.insert(resolved);
                    allFiles.push_back(resolved);
                    worklist.push(resolved);
                }
            }
        }
    }

    // Phase 2: Topological sort so dependencies are processed first
    std::vector<std::string> sorted = topologicalSort(allFiles, deps);

    // Phase 3: Collect symbols in dependency order
    for (const auto& path : sorted)
    {
        auto it = fileMap.find(path);
        if (it != fileMap.end())
        {
            sourceFiles.push_back(it->second);
            parseAndCollectSymbols(it->second);
        }
    }

    std::cout << "Parsed " << sourceFiles.size() << " files" << std::endl;
    std::cout << "Found " << functionMap.size() << " functions" << std::endl;
    std::cout << "Found " << globalVarMap.size() << " global variables" << std::endl;
    std::cout << "Found " << structDefMap.size() << " struct definitions" << std::endl;
}

CTSFunction* CTSModuleSet::getFunction(const std::string& name) const
{
    auto it = functionMap.find(name);
    return (it != functionMap.end()) ? it->second : nullptr;
}

void CTSModuleSet::addExternalFunction(const std::string& name)
{
    if (functionMap.count(name)) return;  // already exists
    TSNode nullNode = {0};
    auto* func = new CTSFunction(name, nullNode, nullptr);
    func->setHasBody(false);
    functionMap[name] = func;
}

CTSGlobalVar* CTSModuleSet::getGlobalVar(const std::string& name) const
{
    auto it = globalVarMap.find(name);
    return (it != globalVarMap.end()) ? it->second : nullptr;
}

CTSStructDef* CTSModuleSet::getStructDef(const std::string& name) const
{
    auto it = structDefMap.find(name);
    return (it != structDefMap.end()) ? it->second : nullptr;
}

//===----------------------------------------------------------------------===//
// Type management
//===----------------------------------------------------------------------===//

void CTSModuleSet::initBuiltinTypes()
{
    auto* ptrType = new SVFPointerType(typeIdCounter++, 8);
    ownedTypes.push_back(ptrType);
    typeMap["ptr"] = ptrType;
    SVFType::setSVFPtrType(ptrType);

    auto* i8Type = new SVFIntegerType(typeIdCounter++, 1);
    i8Type->setSignAndWidth(-8);  // signed 8-bit
    ownedTypes.push_back(i8Type);
    typeMap["i8"] = i8Type;
    typeMap["char"] = i8Type;
    SVFType::setSVFInt8Type(i8Type);

    auto* i32Type = new SVFIntegerType(typeIdCounter++, 4);
    i32Type->setSignAndWidth(-32);  // signed 32-bit
    ownedTypes.push_back(i32Type);
    typeMap["int"] = i32Type;
    typeMap["i32"] = i32Type;

    auto* i64Type = new SVFIntegerType(typeIdCounter++, 8);
    i64Type->setSignAndWidth(-64);  // signed 64-bit
    ownedTypes.push_back(i64Type);
    typeMap["long"] = i64Type;
    typeMap["i64"] = i64Type;

    auto* i16Type = new SVFIntegerType(typeIdCounter++, 2);
    i16Type->setSignAndWidth(-16);  // signed 16-bit
    ownedTypes.push_back(i16Type);
    typeMap["short"] = i16Type;

    auto* voidType = new SVFOtherType(typeIdCounter++, true, 0);
    voidType->setRepr("void");
    ownedTypes.push_back(voidType);
    typeMap["void"] = voidType;

    auto* floatType = new SVFOtherType(typeIdCounter++, true, 4);
    floatType->setRepr("float");
    ownedTypes.push_back(floatType);
    typeMap["float"] = floatType;

    auto* doubleType = new SVFOtherType(typeIdCounter++, true, 8);
    doubleType->setRepr("double");
    ownedTypes.push_back(doubleType);
    typeMap["double"] = doubleType;
}

SVFType* CTSModuleSet::getPtrType() { return typeMap["ptr"]; }
SVFType* CTSModuleSet::getIntType() { return typeMap["int"]; }
SVFType* CTSModuleSet::getInt8Type() { return typeMap["i8"]; }

SVFType* CTSModuleSet::getOrCreateType(const std::string& typeName)
{
    auto it = typeMap.find(typeName);
    if (it != typeMap.end()) return it->second;

    // Check struct types
    if (typeName.find("struct ") == 0)
    {
        std::string structName = typeName.substr(7);
        auto* def = getStructDef(structName);
        if (def && def->getSVFType()) return def->getSVFType();
    }

    // Create as "other" type
    auto* type = new SVFOtherType(typeIdCounter++, true, 1);
    type->setRepr(typeName);
    ownedTypes.push_back(type);
    typeMap[typeName] = type;
    return type;
}

SVFPointerType* CTSModuleSet::createPointerType()
{
    auto* type = new SVFPointerType(typeIdCounter++, 8);
    ownedTypes.push_back(type);
    return type;
}

SVFIntegerType* CTSModuleSet::createIntegerType(u32_t byteSize)
{
    auto* type = new SVFIntegerType(typeIdCounter++, byteSize);
    type->setSignAndWidth(-(short)(byteSize * 8));  // default to signed
    ownedTypes.push_back(type);
    return type;
}

SVFStructType* CTSModuleSet::createStructType(const std::string& name,
                                               const std::vector<const SVFType*>& fields,
                                               u32_t byteSize)
{
    std::vector<const SVFType*> mutableFields = fields;
    auto* type = new SVFStructType(typeIdCounter++, mutableFields, byteSize);
    type->setName(name);
    ownedTypes.push_back(type);
    typeMap["struct " + name] = type;
    return type;
}

SVFArrayType* CTSModuleSet::createArrayType(const SVFType* elemType, u32_t numElements)
{
    auto* type = new SVFArrayType(typeIdCounter++, numElements * elemType->getByteSize());
    type->setTypeOfElement(elemType);
    type->setNumOfElement(numElements);
    ownedTypes.push_back(type);
    return type;
}

//===----------------------------------------------------------------------===//
// Node ID tracking
//===----------------------------------------------------------------------===//

std::pair<CTSSourceFile*, uint32_t> CTSModuleSet::getNodeKey(TSNode node,
                                                              CTSSourceFile* file) const
{
    return std::make_pair(file, ts_node_start_byte(node));
}

void CTSModuleSet::setValID(TSNode node, CTSSourceFile* file, NodeID id)
{
    nodeToValIDMap[getNodeKey(node, file)] = id;
}

NodeID CTSModuleSet::getValID(TSNode node, CTSSourceFile* file) const
{
    auto key = getNodeKey(node, file);
    auto it = nodeToValIDMap.find(key);
    return (it != nodeToValIDMap.end()) ? it->second : (NodeID)-1;
}

void CTSModuleSet::setObjID(TSNode node, CTSSourceFile* file, NodeID id)
{
    nodeToObjIDMap[getNodeKey(node, file)] = id;
}

NodeID CTSModuleSet::getObjID(TSNode node, CTSSourceFile* file) const
{
    auto key = getNodeKey(node, file);
    auto it = nodeToObjIDMap.find(key);
    return (it != nodeToObjIDMap.end()) ? it->second : (NodeID)-1;
}

//===----------------------------------------------------------------------===//
// Symbol collection
//===----------------------------------------------------------------------===//

void CTSModuleSet::parseAndCollectSymbols(CTSSourceFile* file)
{
    TSNode root = file->getRootNode();
    if (ts_node_is_null(root)) return;

    collectFunctions(file, root);
    collectGlobalVars(file, root);
    collectStructDefs(file, root);
}

void CTSModuleSet::collectFunctions(CTSSourceFile* file, TSNode root)
{
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(root, i);
        if (CTSParser::isFunctionDef(child))
        {
            std::string name = CTSParser::getFunctionName(child, file->getSource());
            if (!name.empty())
            {
                auto* func = new CTSFunction(name, child, file);
                func->setHasBody(true);
                func->setBody(CTSParser::getFunctionBody(child));
                func->setParams(CTSParser::getFunctionParams(child));
                addFunction(func);
            }
        }
        else if (CTSParser::isDeclaration(child))
        {
            // Check if this declaration has a function_declarator child
            // e.g. "extern void svf_assert(bool);"
            TSNode declarator = CTSParser::getDeclarator(child);
            if (ts_node_is_null(declarator)) continue;
            if (strcmp(ts_node_type(declarator), "function_declarator") != 0) continue;

            std::string name = CTSParser::getDeclaratorName(declarator, file->getSource());
            if (!name.empty())
            {
                auto* func = new CTSFunction(name, child, file);
                func->setHasBody(false);
                func->setParams(CTSParser::getFunctionDeclParams(declarator));
                addFunction(func);
            }
        }
    }
}

void CTSModuleSet::collectGlobalVars(CTSSourceFile* file, TSNode root)
{
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(root, i);
        if (CTSParser::isDeclaration(child))
        {
            // Handle all declarators (int a, b, c = 1;)
            auto declarators = CTSParser::getDeclarators(child);
            for (TSNode declarator : declarators)
            {
                const char* type = ts_node_type(declarator);
                // Skip function declarations
                if (strcmp(type, "function_declarator") == 0) continue;

                std::string name = CTSParser::getDeclaratorName(declarator, file->getSource());
                if (!name.empty())
                {
                    auto* var = new CTSGlobalVar(name, child, file);
                    var->setTypeNode(CTSParser::getTypeSpecifier(child));
                    if (CTSParser::isInitDeclarator(declarator))
                    {
                        var->setInitializer(CTSParser::getInitializer(declarator));
                    }
                    addGlobalVar(var);
                }
            }
        }
    }
}

void CTSModuleSet::collectStructDefs(CTSSourceFile* file, TSNode root)
{
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(root, i);

        // Find the struct_specifier — either a top-level struct_specifier
        // (standalone struct definition) or embedded in a declaration
        TSNode structSpec = {};
        if (CTSParser::isStructSpecifier(child))
        {
            structSpec = child;
        }
        else if (CTSParser::isDeclaration(child))
        {
            TSNode typeSpec = CTSParser::getTypeSpecifier(child);
            if (!ts_node_is_null(typeSpec) && CTSParser::isStructSpecifier(typeSpec))
                structSpec = typeSpec;
        }

        if (!ts_node_is_null(structSpec))
        {
            std::string name = CTSParser::getStructName(structSpec, file->getSource());
            if (!name.empty())
            {
                auto* structDef = new CTSStructDef(name, structSpec, file);
                std::vector<TSNode> fields = CTSParser::getStructFields(structSpec);
                for (TSNode field : fields)
                {
                    TSNode fieldDecl = CTSParser::getDeclarator(field);
                    if (!ts_node_is_null(fieldDecl))
                    {
                        std::string fieldName = CTSParser::getDeclaratorName(
                            fieldDecl, file->getSource());
                        TSNode fieldType = CTSParser::getTypeSpecifier(field);
                        if (!fieldName.empty())
                        {
                            structDef->addField(fieldName, fieldType, fieldDecl);
                        }
                    }
                }
                addStructDef(structDef);
            }
        }
    }
}

void CTSModuleSet::addFunction(CTSFunction* func)
{
    const std::string& name = func->getName();
    auto it = functionMap.find(name);
    if (it != functionMap.end())
    {
        if (func->hasBody() && !it->second->hasBody())
        {
            delete it->second;
            functionMap[name] = func;
        }
        else
        {
            delete func;
        }
    }
    else
    {
        functionMap[name] = func;
    }
}

void CTSModuleSet::addGlobalVar(CTSGlobalVar* var)
{
    if (globalVarMap.find(var->getName()) == globalVarMap.end())
    {
        globalVarMap[var->getName()] = var;
    }
    else
    {
        delete var;
    }
}

void CTSModuleSet::addStructDef(CTSStructDef* def)
{
    if (!def->getName().empty() && structDefMap.find(def->getName()) == structDefMap.end())
    {
        structDefMap[def->getName()] = def;
    }
    else
    {
        delete def;
    }
}

//===----------------------------------------------------------------------===//
// #include resolution
//===----------------------------------------------------------------------===//

std::vector<std::string> CTSModuleSet::extractIncludes(CTSSourceFile* file)
{
    std::vector<std::string> includes;
    TSNode root = file->getRootNode();
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(root, i);
        if (strcmp(ts_node_type(child), "preproc_include") == 0)
        {
            // Look for the path child node
            TSNode path = ts_node_child_by_field_name(child, "path", 4);
            if (ts_node_is_null(path)) continue;

            const char* pathType = ts_node_type(path);
            if (strcmp(pathType, "string_literal") == 0)
            {
                // #include "file.h" — user include
                std::string text = CTSParser::getNodeText(path, file->getSource());
                // Strip quotes
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                if (!text.empty())
                    includes.push_back(text);
            }
            // Skip system_lib_string (#include <...>) for now
        }
    }
    return includes;
}

std::string CTSModuleSet::resolveInclude(const std::string& include,
                                          const std::string& fromFile)
{
    // 1. Relative to the including file's directory
    fs::path fromDir = fs::path(fromFile).parent_path();
    fs::path candidate = fromDir / include;
    if (fs::exists(candidate))
    {
        try { return fs::canonical(candidate).string(); }
        catch (...) { return candidate.string(); }
    }

    // 2. Check each -I include path
    for (const auto& dir : includePaths)
    {
        candidate = fs::path(dir) / include;
        if (fs::exists(candidate))
        {
            try { return fs::canonical(candidate).string(); }
            catch (...) { return candidate.string(); }
        }
    }

    return "";  // not found
}

std::vector<std::string> CTSModuleSet::topologicalSort(
    const std::vector<std::string>& allFiles,
    const std::map<std::string, std::vector<std::string>>& deps)
{
    // Kahn's algorithm
    // deps[f] = files that f includes (f depends on them)
    // So d must come before f if d is in deps[f]

    // Compute in-degree: how many unprocessed deps each file has
    std::map<std::string, int> inDegree;
    for (const auto& f : allFiles) inDegree[f] = 0;
    for (const auto& pair : deps)
    {
        const auto& f = pair.first;
        const auto& depList = pair.second;
        for (const auto& d : depList)
        {
            if (inDegree.count(d))
                ; // d is a known file
        }
        inDegree[f] = static_cast<int>(depList.size());
    }

    // Reverse adjacency: who depends on me?
    std::map<std::string, std::vector<std::string>> rdeps;
    for (const auto& pair : deps)
    {
        const auto& f = pair.first;
        const auto& depList = pair.second;
        for (const auto& d : depList)
            rdeps[d].push_back(f);
    }

    std::queue<std::string> q;
    for (const auto& pair : inDegree)
    {
        if (pair.second == 0) q.push(pair.first);
    }

    std::vector<std::string> result;
    while (!q.empty())
    {
        auto f = q.front();
        q.pop();
        result.push_back(f);
        for (const auto& dep : rdeps[f])
        {
            if (--inDegree[dep] == 0) q.push(dep);
        }
    }

    // Any files not in result are in cycles — append them at end
    if (result.size() < allFiles.size())
    {
        std::set<std::string> inResult(result.begin(), result.end());
        for (const auto& f : allFiles)
        {
            if (inResult.find(f) == inResult.end())
                result.push_back(f);
        }
    }

    return result;
}

} // namespace SVF
