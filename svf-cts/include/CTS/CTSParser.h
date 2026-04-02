#ifndef CTS_PARSER_H
#define CTS_PARSER_H

#include <tree_sitter/api.h>
#include <string>
#include <vector>

namespace SVF
{

/// Wraps tree-sitter C grammar for parsing C source files
class CTSParser
{
public:
    CTSParser();
    ~CTSParser();

    /// Parse a source string, returns the tree (caller must call ts_tree_delete)
    TSTree* parse(const std::string& source);

    /// Get the tree-sitter C language
    static const TSLanguage* getLanguage();

    //===------------------------------------------------------------------===//
    // Node type identification
    //===------------------------------------------------------------------===//

    static bool isFunctionDef(TSNode node);
    static bool isDeclaration(TSNode node);
    static bool isAssignmentExpr(TSNode node);
    static bool isCallExpr(TSNode node);
    static bool isPointerDeref(TSNode node);
    static bool isAddressOf(TSNode node);
    static bool isFieldExpr(TSNode node);
    static bool isSubscriptExpr(TSNode node);
    static bool isReturnStmt(TSNode node);
    static bool isIfStmt(TSNode node);
    static bool isWhileStmt(TSNode node);
    static bool isForStmt(TSNode node);
    static bool isCompoundStmt(TSNode node);
    static bool isExprStmt(TSNode node);
    static bool isInitDeclarator(TSNode node);
    static bool isStructSpecifier(TSNode node);

    //===------------------------------------------------------------------===//
    // Node information extraction
    //===------------------------------------------------------------------===//

    /// Get text of a node from source
    static std::string getNodeText(TSNode node, const std::string& source);

    /// Get function name from function_definition node
    static std::string getFunctionName(TSNode funcDef, const std::string& source);

    /// Get function body (compound_statement)
    static TSNode getFunctionBody(TSNode funcDef);

    /// Get function parameters from function_definition
    static std::vector<TSNode> getFunctionParams(TSNode funcDef);

    /// Get function parameters from a function_declarator node (for declarations)
    static std::vector<TSNode> getFunctionDeclParams(TSNode funcDeclarator);

    /// Get declarator from declaration (first only — for backward compat)
    static TSNode getDeclarator(TSNode decl);

    /// Get ALL declarators from declaration (for multi-declarator: int a, b, c;)
    static std::vector<TSNode> getDeclarators(TSNode decl);

    /// Get declarator name
    static std::string getDeclaratorName(TSNode declarator, const std::string& source);

    /// Get type specifier from declaration
    static TSNode getTypeSpecifier(TSNode decl);

    /// Get initializer from init_declarator
    static TSNode getInitializer(TSNode initDecl);

    /// Get return value from return_statement
    static TSNode getReturnValue(TSNode retStmt);

    /// Get loop body
    static TSNode getLoopBody(TSNode loopStmt);

    /// Get struct name
    static std::string getStructName(TSNode structSpec, const std::string& source);

    /// Get struct fields
    static std::vector<TSNode> getStructFields(TSNode structSpec);

    /// Format source location string matching SVF/LLVM format
    /// Returns: { "ln": LINE, "cl": COL, "fl": "FILEPATH" }
    static std::string formatSourceLoc(TSNode node, const std::string& filePath);

    /// Get array dimension from array_declarator node. Returns 0 if not constant.
    static unsigned getArraySize(TSNode arrayDeclarator, const std::string& source);

private:
    TSParser* parser;
};

} // namespace SVF

#endif // CTS_PARSER_H
