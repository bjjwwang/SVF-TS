#ifndef CTS_SVFIR_BUILDER_H
#define CTS_SVFIR_BUILDER_H

#include "CTS/CTSModule.h"
#include "CTS/CTSICFGBuilder.h"
#include "CTS/ScopeManager.h"
#include "CTS/SSABuilder.h"
#include "SVFIR/SVFIR.h"

#include <tree_sitter/api.h>
#include <memory>

namespace SVF
{

/// Builds SVFIR (PAG) from TreeSitter C AST.
/// This class is declared as a friend of SVFIR to access private add*Stmt methods.
class CTSSVFIRBuilder
{
public:
    CTSSVFIRBuilder();
    ~CTSSVFIRBuilder();

    /// Main entry point: build SVFIR from source files
    SVFIR* build(const std::vector<std::string>& sourceFiles);

private:
    //===------------------------------------------------------------------===//
    // Node creation (using proper SVF node types via friend access)
    //===------------------------------------------------------------------===//

    /// Create a StackObjVar + ValVar pair for a local variable, with AddrStmt
    /// Returns the ValVar node ID
    NodeID createLocalVar(const std::string& name, TSNode node, CTSSourceFile* file,
                          const SVFType* type, const ICFGNode* icfgNode);

    /// Create a GlobalObjVar + GlobalValVar pair
    NodeID createGlobalVar(const std::string& name, TSNode node, CTSSourceFile* file,
                           const SVFType* type);

    /// Create a HeapObjVar for malloc/calloc allocation
    NodeID createHeapObj(TSNode allocSite, CTSSourceFile* file, const ICFGNode* icfgNode);

    /// Create a new ValVar (for SSA versions, expression temporaries, etc.)
    NodeID createValNode(const SVFType* type, const ICFGNode* icfgNode);

    /// Create a constant integer value node
    NodeID createConstIntNode(s64_t value, const ICFGNode* icfgNode);

    /// Create a constant null pointer node
    NodeID createConstNullNode(const ICFGNode* icfgNode);

    //===------------------------------------------------------------------===//
    // Edge creation (via friend access to SVFIR private methods)
    //===------------------------------------------------------------------===//

    void addAddrEdge(NodeID src, NodeID dst);
    void addCopyEdge(NodeID src, NodeID dst);
    void addLoadEdge(NodeID src, NodeID dst);
    void addStoreEdge(NodeID src, NodeID dst, const ICFGNode* icfgNode);
    void addGepEdge(NodeID src, NodeID dst, const AccessPath& ap, bool constGep = true);
    void addCallEdge(NodeID src, NodeID dst, const CallICFGNode* cs,
                     const FunEntryICFGNode* entry);
    void addRetEdge(NodeID src, NodeID dst, const CallICFGNode* cs,
                    const FunExitICFGNode* exit);
    void addBinaryOPEdge(NodeID op1, NodeID op2, NodeID dst, u32_t opcode);
    void addCmpEdge(NodeID op1, NodeID op2, NodeID dst, u32_t predicate);
    void addUnaryOPEdge(NodeID src, NodeID dst, u32_t opcode);
    void addBranchEdge(NodeID br, NodeID cond,
                       const BranchStmt::SuccAndCondPairVec& succs);

    //===------------------------------------------------------------------===//
    // Build pipeline
    //===------------------------------------------------------------------===//

    void initSpecialNodes();
    void createFunctionObjects();
    void createReturnNodes();
    void processGlobalVars();
    void processFunction(CTSFunction* func);
    void processCompoundStmt(TSNode compound, CTSSourceFile* file);
    void processStatement(TSNode stmt, CTSSourceFile* file);
    void processDeclaration(TSNode decl, CTSSourceFile* file);
    void processExpression(TSNode expr, CTSSourceFile* file);
    void processAssignment(TSNode assign, CTSSourceFile* file);
    NodeID processCallExpr(TSNode call, CTSSourceFile* file);
    void processReturn(TSNode ret, CTSSourceFile* file);
    void processIfStatement(TSNode ifStmt, CTSSourceFile* file);
    void processWhileStatement(TSNode whileStmt, CTSSourceFile* file);
    void processForStatement(TSNode forStmt, CTSSourceFile* file);
    void processSwitchStatement(TSNode switchStmt, CTSSourceFile* file);
    void processDoWhileStatement(TSNode doStmt, CTSSourceFile* file);

    //===------------------------------------------------------------------===//
    // Expression evaluation
    //===------------------------------------------------------------------===//

    /// Get the value (rvalue) of an expression - returns a NodeID
    NodeID getExprValue(TSNode expr, CTSSourceFile* file);

    /// Get the address (lvalue) of an expression - for store targets
    NodeID getExprLValue(TSNode expr, CTSSourceFile* file);

    /// Compute GEP node for struct field access (s.x or s->f)
    NodeID getFieldGepNode(TSNode fieldExpr, CTSSourceFile* file);

    /// Compute GEP node for array subscript (arr[i])
    NodeID getArrayGepNode(TSNode subscriptExpr, CTSSourceFile* file);

    //===------------------------------------------------------------------===//
    // Type resolution and StInfo
    //===------------------------------------------------------------------===//

    /// Resolve a C type specifier node to SVFType
    const SVFType* resolveType(TSNode typeSpec, CTSSourceFile* file);

    /// Resolve full type including pointer/array from declarator
    const SVFType* resolveFullType(TSNode typeSpec, TSNode declarator,
                                    CTSSourceFile* file);

    /// Build struct types bottom-up with real field types
    void buildStructTypes();

    /// Create StInfo for types (following LLVM frontend pattern)
    StInfo* collectTypeInfo(const SVFType* type);
    StInfo* collectStructInfo(const SVFStructType* st);
    StInfo* collectArrayInfo(const SVFArrayType* at);
    StInfo* collectSimpleTypeInfo(const SVFType* type);

    /// Create StInfo for all types and register with PAG
    void buildTypeInfo();

    //===------------------------------------------------------------------===//
    // Helpers
    //===------------------------------------------------------------------===//

    /// Get ICFG node for a statement
    ICFGNode* getICFGNode(TSNode stmt, CTSSourceFile* file);

    //===------------------------------------------------------------------===//
    // State
    //===------------------------------------------------------------------===//

    SVFIR* pag;
    CTSModuleSet* moduleSet;
    CTSICFGBuilder* icfgBuilder;
    std::unique_ptr<ScopeManager> scopeManager;
    std::unique_ptr<SSABuilder> ssaBuilder;

    /// Current context
    CTSFunction* currentFunc;
    const SVFBasicBlock* currentBB;
    ICFGNode* currentICFGNode;

    /// Special node IDs
    NodeID blackHoleNode;
    NodeID constPtrNode;
    NodeID nullPtrNode;

    /// Type → StInfo cache
    std::map<const SVFType*, StInfo*> type2StInfo;
};

} // namespace SVF

#endif // CTS_SVFIR_BUILDER_H
