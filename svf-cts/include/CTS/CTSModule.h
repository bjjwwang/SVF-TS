#ifndef CTS_MODULE_H
#define CTS_MODULE_H

#include "CTS/CTSParser.h"
#include "SVFIR/SVFType.h"
#include "Graphs/ICFGNode.h"

#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <memory>
#include <filesystem>

namespace SVF
{

class FunObjVar;
class SVFBasicBlock;

//===----------------------------------------------------------------------===//
// CTSSourceFile - represents a parsed C source file
//===----------------------------------------------------------------------===//
class CTSSourceFile
{
public:
    CTSSourceFile(const std::string& path);
    ~CTSSourceFile();

    bool parse();

    const std::string& getFilePath() const { return filePath; }
    const std::string& getSource() const { return source; }
    TSNode getRootNode() const;
    std::string getNodeText(TSNode node) const;

private:
    std::string filePath;
    std::string source;
    TSTree* tree;
    std::unique_ptr<CTSParser> parser;
};

//===----------------------------------------------------------------------===//
// CTSFunction - represents a C function
//===----------------------------------------------------------------------===//
class CTSFunction
{
public:
    CTSFunction(const std::string& name, TSNode node, CTSSourceFile* file);

    const std::string& getName() const { return name; }
    TSNode getNode() const { return node; }
    CTSSourceFile* getSourceFile() const { return sourceFile; }

    bool hasBody() const { return hasDefinition; }
    void setHasBody(bool b) { hasDefinition = b; }

    TSNode getBody() const { return body; }
    void setBody(TSNode b) { body = b; }

    const std::vector<TSNode>& getParams() const { return params; }
    void setParams(const std::vector<TSNode>& p) { params = p; }

    const FunObjVar* getFunObjVar() const { return funObjVar; }
    void setFunObjVar(const FunObjVar* f) { funObjVar = f; }

private:
    std::string name;
    TSNode node;
    CTSSourceFile* sourceFile;
    bool hasDefinition;
    TSNode body;
    std::vector<TSNode> params;
    const FunObjVar* funObjVar;
};

//===----------------------------------------------------------------------===//
// CTSGlobalVar - represents a global variable
//===----------------------------------------------------------------------===//
class CTSGlobalVar
{
public:
    CTSGlobalVar(const std::string& name, TSNode node, CTSSourceFile* file);

    const std::string& getName() const { return name; }
    TSNode getNode() const { return node; }
    CTSSourceFile* getSourceFile() const { return sourceFile; }

    TSNode getTypeNode() const { return typeNode; }
    void setTypeNode(TSNode t) { typeNode = t; }

    TSNode getInitializer() const { return initializer; }
    void setInitializer(TSNode i) { initializer = i; }

private:
    std::string name;
    TSNode node;
    CTSSourceFile* sourceFile;
    TSNode typeNode;
    TSNode initializer;
};

//===----------------------------------------------------------------------===//
// CTSStructDef - represents a struct definition
//===----------------------------------------------------------------------===//
class CTSStructDef
{
public:
    CTSStructDef(const std::string& name, TSNode node, CTSSourceFile* file);

    const std::string& getName() const { return name; }
    CTSSourceFile* getSourceFile() const { return sourceFile; }
    void addField(const std::string& fieldName, TSNode typeNode, TSNode declarator);
    int getFieldIndex(const std::string& fieldName) const;
    size_t getFieldCount() const { return fieldNames.size(); }
    const std::vector<std::string>& getFieldNames() const { return fieldNames; }
    const std::vector<TSNode>& getFieldTypeNodes() const { return fieldTypes; }
    const std::vector<TSNode>& getFieldDeclarators() const { return fieldDeclarators; }

    SVFType* getSVFType() const { return svfType; }
    void setSVFType(SVFType* t) { svfType = t; }

private:
    std::string name;
    TSNode node;
    CTSSourceFile* sourceFile;
    std::vector<std::string> fieldNames;
    std::vector<TSNode> fieldTypes;
    std::vector<TSNode> fieldDeclarators;
    SVFType* svfType;
};

//===----------------------------------------------------------------------===//
// CTSModuleSet - singleton managing all source files and symbols
//===----------------------------------------------------------------------===//
class CTSModuleSet
{
public:
    static CTSModuleSet* getModuleSet();
    static void releaseModuleSet();

    /// Parse and collect symbols from source files
    void buildFromFiles(const std::vector<std::string>& files);

    /// Include path management
    void setIncludePaths(const std::vector<std::string>& paths) { includePaths = paths; }
    void addIncludePath(const std::string& path) { includePaths.push_back(path); }

    /// Accessors
    const std::vector<CTSSourceFile*>& getSourceFiles() const { return sourceFiles; }
    const std::map<std::string, CTSFunction*>& getFunctions() const { return functionMap; }
    const std::map<std::string, CTSGlobalVar*>& getGlobalVars() const { return globalVarMap; }
    const std::map<std::string, CTSStructDef*>& getStructDefs() const { return structDefMap; }

    CTSFunction* getFunction(const std::string& name) const;
    CTSGlobalVar* getGlobalVar(const std::string& name) const;
    CTSStructDef* getStructDef(const std::string& name) const;

    /// Type management
    SVFType* getPtrType();
    SVFType* getIntType();
    SVFType* getInt8Type();
    SVFType* getOrCreateType(const std::string& typeName);
    SVFPointerType* createPointerType();
    SVFIntegerType* createIntegerType(u32_t byteSize);
    SVFStructType* createStructType(const std::string& name,
                                     const std::vector<const SVFType*>& fields,
                                     u32_t byteSize = 0);
    SVFArrayType* createArrayType(const SVFType* elemType, u32_t numElements);

    /// Type ID counter (for creating new types)
    u32_t getTypeIdCounter() { return typeIdCounter++; }

    /// Add an externally created type to our ownership
    void addOwnedType(SVFType* type) { ownedTypes.push_back(type); }

    /// Node ID tracking
    void setValID(TSNode node, CTSSourceFile* file, NodeID id);
    NodeID getValID(TSNode node, CTSSourceFile* file) const;
    void setObjID(TSNode node, CTSSourceFile* file, NodeID id);
    NodeID getObjID(TSNode node, CTSSourceFile* file) const;

private:
    CTSModuleSet();
    ~CTSModuleSet();

    void initBuiltinTypes();
    void parseAndCollectSymbols(CTSSourceFile* file);
    void collectFunctions(CTSSourceFile* file, TSNode root);
    void collectGlobalVars(CTSSourceFile* file, TSNode root);
    void collectStructDefs(CTSSourceFile* file, TSNode root);
    void addFunction(CTSFunction* func);
    void addGlobalVar(CTSGlobalVar* var);
    void addStructDef(CTSStructDef* def);

    /// #include resolution
    std::vector<std::string> extractIncludes(CTSSourceFile* file);
    std::string resolveInclude(const std::string& include,
                                const std::string& fromFile);
    std::vector<std::string> topologicalSort(
        const std::vector<std::string>& allFiles,
        const std::map<std::string, std::vector<std::string>>& deps);

    std::pair<CTSSourceFile*, uint32_t> getNodeKey(TSNode node, CTSSourceFile* file) const;

    static CTSModuleSet* instance;

    std::vector<CTSSourceFile*> sourceFiles;
    std::map<std::string, CTSFunction*> functionMap;
    std::map<std::string, CTSGlobalVar*> globalVarMap;
    std::map<std::string, CTSStructDef*> structDefMap;

    /// Type management
    std::map<std::string, SVFType*> typeMap;
    std::vector<SVFType*> ownedTypes;
    u32_t typeIdCounter;

    /// Include handling
    std::vector<std::string> includePaths;
    std::set<std::string> parsedFiles;

    /// Node ID maps: (file, byte_offset) → NodeID
    std::map<std::pair<CTSSourceFile*, uint32_t>, NodeID> nodeToValIDMap;
    std::map<std::pair<CTSSourceFile*, uint32_t>, NodeID> nodeToObjIDMap;
};

} // namespace SVF

#endif // CTS_MODULE_H
