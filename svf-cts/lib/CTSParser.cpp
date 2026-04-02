#include "CTS/CTSParser.h"
#include <cstring>

// Declared by tree-sitter-c grammar
extern "C" const TSLanguage* tree_sitter_c(void);

namespace SVF
{

CTSParser::CTSParser()
{
    parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_c());
}

CTSParser::~CTSParser()
{
    if (parser)
    {
        ts_parser_delete(parser);
    }
}

TSTree* CTSParser::parse(const std::string& source)
{
    return ts_parser_parse_string(parser, nullptr, source.c_str(), source.length());
}

const TSLanguage* CTSParser::getLanguage()
{
    return tree_sitter_c();
}

//===----------------------------------------------------------------------===//
// Node type identification
//===----------------------------------------------------------------------===//

static inline bool nodeTypeIs(TSNode node, const char* type)
{
    if (ts_node_is_null(node)) return false;
    return strcmp(ts_node_type(node), type) == 0;
}

bool CTSParser::isFunctionDef(TSNode node) { return nodeTypeIs(node, "function_definition"); }
bool CTSParser::isDeclaration(TSNode node) { return nodeTypeIs(node, "declaration"); }
bool CTSParser::isAssignmentExpr(TSNode node) { return nodeTypeIs(node, "assignment_expression"); }
bool CTSParser::isCallExpr(TSNode node) { return nodeTypeIs(node, "call_expression"); }
bool CTSParser::isPointerDeref(TSNode node) { return nodeTypeIs(node, "pointer_expression"); }
bool CTSParser::isFieldExpr(TSNode node) { return nodeTypeIs(node, "field_expression"); }
bool CTSParser::isSubscriptExpr(TSNode node) { return nodeTypeIs(node, "subscript_expression"); }
bool CTSParser::isReturnStmt(TSNode node) { return nodeTypeIs(node, "return_statement"); }
bool CTSParser::isIfStmt(TSNode node) { return nodeTypeIs(node, "if_statement"); }
bool CTSParser::isWhileStmt(TSNode node) { return nodeTypeIs(node, "while_statement"); }
bool CTSParser::isForStmt(TSNode node) { return nodeTypeIs(node, "for_statement"); }
bool CTSParser::isCompoundStmt(TSNode node) { return nodeTypeIs(node, "compound_statement"); }
bool CTSParser::isExprStmt(TSNode node) { return nodeTypeIs(node, "expression_statement"); }
bool CTSParser::isInitDeclarator(TSNode node) { return nodeTypeIs(node, "init_declarator"); }
bool CTSParser::isStructSpecifier(TSNode node) { return nodeTypeIs(node, "struct_specifier"); }

bool CTSParser::isAddressOf(TSNode node)
{
    if (!nodeTypeIs(node, "unary_expression")) return false;
    TSNode op = ts_node_child(node, 0);
    if (ts_node_is_null(op)) return false;
    const char* text = ts_node_type(op);
    // The operator child is a literal "&" token
    return text && strcmp(text, "&") == 0;
}

//===----------------------------------------------------------------------===//
// Node information extraction
//===----------------------------------------------------------------------===//

std::string CTSParser::getNodeText(TSNode node, const std::string& source)
{
    if (ts_node_is_null(node)) return "";
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source.size() || end > source.size()) return "";
    return source.substr(start, end - start);
}

std::string CTSParser::getFunctionName(TSNode funcDef, const std::string& source)
{
    // function_definition: type declarator body
    // declarator is a function_declarator containing the name
    TSNode declarator = ts_node_child_by_field_name(funcDef, "declarator", 10);
    if (ts_node_is_null(declarator)) return "";

    // If it's a function_declarator, get its declarator child (the name)
    if (nodeTypeIs(declarator, "function_declarator"))
    {
        TSNode nameNode = ts_node_child_by_field_name(declarator, "declarator", 10);
        if (!ts_node_is_null(nameNode))
        {
            // Could be a pointer_declarator wrapping the name
            if (nodeTypeIs(nameNode, "pointer_declarator"))
            {
                TSNode inner = ts_node_child_by_field_name(nameNode, "declarator", 10);
                if (!ts_node_is_null(inner)) return getNodeText(inner, source);
            }
            return getNodeText(nameNode, source);
        }
    }
    // If declarator is a pointer_declarator, drill deeper
    else if (nodeTypeIs(declarator, "pointer_declarator"))
    {
        TSNode inner = ts_node_child_by_field_name(declarator, "declarator", 10);
        if (!ts_node_is_null(inner) && nodeTypeIs(inner, "function_declarator"))
        {
            TSNode nameNode = ts_node_child_by_field_name(inner, "declarator", 10);
            if (!ts_node_is_null(nameNode)) return getNodeText(nameNode, source);
        }
    }

    return getNodeText(declarator, source);
}

TSNode CTSParser::getFunctionBody(TSNode funcDef)
{
    return ts_node_child_by_field_name(funcDef, "body", 4);
}

std::vector<TSNode> CTSParser::getFunctionParams(TSNode funcDef)
{
    std::vector<TSNode> params;
    TSNode declarator = ts_node_child_by_field_name(funcDef, "declarator", 10);
    if (ts_node_is_null(declarator)) return params;

    // Navigate to function_declarator
    TSNode funcDecl = declarator;
    if (nodeTypeIs(declarator, "pointer_declarator"))
    {
        funcDecl = ts_node_child_by_field_name(declarator, "declarator", 10);
    }
    if (ts_node_is_null(funcDecl) || !nodeTypeIs(funcDecl, "function_declarator"))
        return params;

    // Get parameter_list
    TSNode paramList = ts_node_child_by_field_name(funcDecl, "parameters", 10);
    if (ts_node_is_null(paramList)) return params;

    uint32_t count = ts_node_named_child_count(paramList);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode param = ts_node_named_child(paramList, i);
        if (nodeTypeIs(param, "parameter_declaration"))
        {
            params.push_back(param);
        }
    }

    return params;
}

std::vector<TSNode> CTSParser::getFunctionDeclParams(TSNode funcDeclarator)
{
    std::vector<TSNode> params;
    if (ts_node_is_null(funcDeclarator)) return params;

    // funcDeclarator is already a function_declarator node
    TSNode paramList = ts_node_child_by_field_name(funcDeclarator, "parameters", 10);
    if (ts_node_is_null(paramList)) return params;

    uint32_t count = ts_node_named_child_count(paramList);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode param = ts_node_named_child(paramList, i);
        if (nodeTypeIs(param, "parameter_declaration"))
        {
            params.push_back(param);
        }
    }

    return params;
}

TSNode CTSParser::getDeclarator(TSNode decl)
{
    // A declaration has: type_specifier, declarator(s)
    uint32_t count = ts_node_named_child_count(decl);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(decl, i);
        const char* type = ts_node_type(child);
        if (strcmp(type, "init_declarator") == 0 ||
            strcmp(type, "pointer_declarator") == 0 ||
            strcmp(type, "array_declarator") == 0 ||
            strcmp(type, "function_declarator") == 0 ||
            strcmp(type, "identifier") == 0)
        {
            return child;
        }
    }
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    return nullNode;
}

std::vector<TSNode> CTSParser::getDeclarators(TSNode decl)
{
    std::vector<TSNode> result;
    uint32_t count = ts_node_named_child_count(decl);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(decl, i);
        const char* type = ts_node_type(child);
        if (strcmp(type, "init_declarator") == 0 ||
            strcmp(type, "pointer_declarator") == 0 ||
            strcmp(type, "array_declarator") == 0 ||
            strcmp(type, "function_declarator") == 0 ||
            strcmp(type, "identifier") == 0)
        {
            result.push_back(child);
        }
    }
    return result;
}

std::string CTSParser::getDeclaratorName(TSNode declarator, const std::string& source)
{
    if (ts_node_is_null(declarator)) return "";

    const char* type = ts_node_type(declarator);

    if (strcmp(type, "identifier") == 0)
    {
        return getNodeText(declarator, source);
    }

    if (strcmp(type, "init_declarator") == 0)
    {
        // init_declarator: declarator "=" value
        TSNode inner = ts_node_child_by_field_name(declarator, "declarator", 10);
        return getDeclaratorName(inner, source);
    }

    if (strcmp(type, "pointer_declarator") == 0)
    {
        TSNode inner = ts_node_child_by_field_name(declarator, "declarator", 10);
        return getDeclaratorName(inner, source);
    }

    if (strcmp(type, "array_declarator") == 0)
    {
        TSNode inner = ts_node_child_by_field_name(declarator, "declarator", 10);
        return getDeclaratorName(inner, source);
    }

    if (strcmp(type, "function_declarator") == 0)
    {
        TSNode inner = ts_node_child_by_field_name(declarator, "declarator", 10);
        return getDeclaratorName(inner, source);
    }

    return getNodeText(declarator, source);
}

TSNode CTSParser::getTypeSpecifier(TSNode decl)
{
    // First named child is typically the type specifier
    uint32_t count = ts_node_named_child_count(decl);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(decl, i);
        const char* type = ts_node_type(child);
        if (strcmp(type, "primitive_type") == 0 ||
            strcmp(type, "sized_type_specifier") == 0 ||
            strcmp(type, "struct_specifier") == 0 ||
            strcmp(type, "type_identifier") == 0 ||
            strcmp(type, "enum_specifier") == 0)
        {
            return child;
        }
    }
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    return nullNode;
}

TSNode CTSParser::getInitializer(TSNode initDecl)
{
    if (!nodeTypeIs(initDecl, "init_declarator"))
    {
        TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
        return nullNode;
    }
    return ts_node_child_by_field_name(initDecl, "value", 5);
}

TSNode CTSParser::getReturnValue(TSNode retStmt)
{
    if (ts_node_named_child_count(retStmt) > 0)
    {
        return ts_node_named_child(retStmt, 0);
    }
    TSNode nullNode = {{0, 0, 0, 0}, nullptr, nullptr};
    return nullNode;
}

TSNode CTSParser::getLoopBody(TSNode loopStmt)
{
    return ts_node_child_by_field_name(loopStmt, "body", 4);
}

std::string CTSParser::getStructName(TSNode structSpec, const std::string& source)
{
    TSNode name = ts_node_child_by_field_name(structSpec, "name", 4);
    if (!ts_node_is_null(name))
    {
        return getNodeText(name, source);
    }
    return "";
}

std::vector<TSNode> CTSParser::getStructFields(TSNode structSpec)
{
    std::vector<TSNode> fields;
    TSNode body = ts_node_child_by_field_name(structSpec, "body", 4);
    if (ts_node_is_null(body)) return fields;

    uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; i++)
    {
        TSNode child = ts_node_named_child(body, i);
        if (nodeTypeIs(child, "field_declaration"))
        {
            fields.push_back(child);
        }
    }
    return fields;
}

std::string CTSParser::formatSourceLoc(TSNode node, const std::string& filePath)
{
    if (ts_node_is_null(node) || filePath.empty()) return "";
    TSPoint start = ts_node_start_point(node);
    // tree-sitter is 0-indexed, LLVM/SVF uses 1-indexed
    return "{ \"ln\": " + std::to_string(start.row + 1)
         + ", \"cl\": " + std::to_string(start.column + 1)
         + ", \"fl\": \"" + filePath + "\" }";
}

unsigned CTSParser::getArraySize(TSNode arrayDeclarator, const std::string& source)
{
    TSNode sizeNode = ts_node_child_by_field_name(arrayDeclarator, "size", 4);
    if (ts_node_is_null(sizeNode)) return 0;
    std::string sizeText = getNodeText(sizeNode, source);
    try { return static_cast<unsigned>(std::stoul(sizeText)); }
    catch (...) { return 0; }
}

} // namespace SVF
