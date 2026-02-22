/**
 * @file   cli_xml_parser.c
 * @brief  CLI XML 配置文件解析
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "cfg"

#include "cli_xml_parser.h"

#include <ctype.h>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_element.h"
#include "cli_tree.h"
#include "cli_view.h"
#include "errcode.h"
#include "log.h"

static void merge_global_to_views(cli_view_node_t *view, cli_tree_node_t *global_tree);

// ============================================================================
// Expression AST - supports [ A | B ] (optional) and { A | B } (required)
// ============================================================================

// AST node types
typedef enum
{
    EXPR_NODE_ELEMENT,  // Single element ID
    EXPR_NODE_SEQUENCE, // Sequential list of items
    EXPR_NODE_OPTIONAL, // [ alt1 | alt2 | ... ] — match 0 or 1
    EXPR_NODE_REQUIRED, // { alt1 | alt2 | ... } — match exactly 1
} expr_node_type_t;

// AST node
typedef struct expr_node
{
    expr_node_type_t type;
    uint32_t element_id;         // For EXPR_NODE_ELEMENT
    struct expr_node **children; // For SEQUENCE/OPTIONAL/REQUIRED
    uint32_t num_children;
    uint32_t children_capacity;
} expr_node_t;

// Expression token types
typedef enum
{
    EXPR_TOK_NUMBER,   // Element ID number
    EXPR_TOK_LBRACKET, // [
    EXPR_TOK_RBRACKET, // ]
    EXPR_TOK_LBRACE,   // {
    EXPR_TOK_RBRACE,   // }
    EXPR_TOK_PIPE,     // |
    EXPR_TOK_END,      // End of expression
} expr_tok_type_t;

// Expression token
typedef struct
{
    expr_tok_type_t type;
    uint32_t value; // For NUMBER tokens
} expr_token_t;

// Tokenizer state
typedef struct
{
    const char *input;
    uint32_t pos;
} expr_tokenizer_t;

// Create AST node
static expr_node_t *expr_node_create(expr_node_type_t type)
{
    expr_node_t *node = g_malloc0(sizeof(expr_node_t));
    node->type = type;
    return node;
}

// Add child to AST node
static void expr_node_add_child(expr_node_t *parent, expr_node_t *child)
{
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_cap = parent->children_capacity == 0 ? 4 : parent->children_capacity * 2;
        parent->children = realloc(parent->children, new_cap * sizeof(expr_node_t *));
        parent->children_capacity = new_cap;
    }
    parent->children[parent->num_children++] = child;
}

// Free AST
static void expr_node_free(expr_node_t *node)
{
    if (!node)
    {
        return;
    }
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        expr_node_free(node->children[i]);
    }
    g_free(node->children);
    g_free(node);
}

// Get next token from expression string
static expr_token_t expr_next_token(expr_tokenizer_t *tok)
{
    expr_token_t token = {.type = EXPR_TOK_END, .value = 0};

    // Skip whitespace
    while (tok->input[tok->pos] && (tok->input[tok->pos] == ' ' || tok->input[tok->pos] == '\t'))
    {
        tok->pos++;
    }

    if (!tok->input[tok->pos])
    {
        return token;
    }

    char c = tok->input[tok->pos];
    switch (c)
    {
        case '[':
            token.type = EXPR_TOK_LBRACKET;
            tok->pos++;
            break;
        case ']':
            token.type = EXPR_TOK_RBRACKET;
            tok->pos++;
            break;
        case '{':
            token.type = EXPR_TOK_LBRACE;
            tok->pos++;
            break;
        case '}':
            token.type = EXPR_TOK_RBRACE;
            tok->pos++;
            break;
        case '|':
            token.type = EXPR_TOK_PIPE;
            tok->pos++;
            break;
        default:
            if (c >= '0' && c <= '9')
            {
                token.type = EXPR_TOK_NUMBER;
                token.value = 0;
                while (tok->input[tok->pos] >= '0' && tok->input[tok->pos] <= '9')
                {
                    token.value = token.value * 10 + (tok->input[tok->pos] - '0');
                    tok->pos++;
                }
            }
            else
            {
                // Skip unknown character
                tok->pos++;
                return expr_next_token(tok);
            }
            break;
    }

    return token;
}

// Peek at next token without consuming
static expr_token_t expr_peek_token(expr_tokenizer_t *tok)
{
    uint32_t saved_pos = tok->pos;
    expr_token_t token = expr_next_token(tok);
    tok->pos = saved_pos;
    return token;
}

// Forward declarations for recursive descent parser
static expr_node_t *parse_expr(expr_tokenizer_t *tok);

// Parse alternatives: expr ('|' expr)*
static expr_node_t *parse_alternatives(expr_tokenizer_t *tok, expr_node_type_t group_type, expr_tok_type_t end_tok)
{
    expr_node_t *group = expr_node_create(group_type);

    // Parse first alternative
    expr_node_t *alt = parse_expr(tok);
    if (alt)
    {
        expr_node_add_child(group, alt);
    }

    // Parse remaining alternatives separated by '|'
    while (1)
    {
        expr_token_t peek = expr_peek_token(tok);
        if (peek.type == EXPR_TOK_PIPE)
        {
            expr_next_token(tok); // consume '|'
            alt = parse_expr(tok);
            if (alt)
            {
                expr_node_add_child(group, alt);
            }
        }
        else
        {
            break;
        }
    }

    // Consume closing bracket/brace
    expr_token_t close = expr_peek_token(tok);
    if (close.type == end_tok)
    {
        expr_next_token(tok);
    }

    return group;
}

// Parse expression: item+
// An item is: NUMBER | '[' alternatives ']' | '{' alternatives '}'
static expr_node_t *parse_expr(expr_tokenizer_t *tok)
{
    expr_node_t *seq = expr_node_create(EXPR_NODE_SEQUENCE);

    while (1)
    {
        expr_token_t peek = expr_peek_token(tok);

        if (peek.type == EXPR_TOK_NUMBER)
        {
            expr_next_token(tok); // consume
            expr_node_t *elem = expr_node_create(EXPR_NODE_ELEMENT);
            elem->element_id = peek.value;
            expr_node_add_child(seq, elem);
        }
        else if (peek.type == EXPR_TOK_LBRACKET)
        {
            expr_next_token(tok); // consume '['
            expr_node_t *opt = parse_alternatives(tok, EXPR_NODE_OPTIONAL, EXPR_TOK_RBRACKET);
            if (opt)
            {
                expr_node_add_child(seq, opt);
            }
        }
        else if (peek.type == EXPR_TOK_LBRACE)
        {
            expr_next_token(tok); // consume '{'
            expr_node_t *req = parse_alternatives(tok, EXPR_NODE_REQUIRED, EXPR_TOK_RBRACE);
            if (req)
            {
                expr_node_add_child(seq, req);
            }
        }
        else
        {
            // End of expression or closing bracket/brace/pipe
            break;
        }
    }

    // Simplify: if sequence has exactly one child, return the child directly
    if (seq->num_children == 1)
    {
        expr_node_t *child = seq->children[0];
        seq->children[0] = NULL;
        seq->num_children = 0;
        expr_node_free(seq);
        return child;
    }

    if (seq->num_children == 0)
    {
        expr_node_free(seq);
        return NULL;
    }

    return seq;
}

// ============================================================================
// Tree building from AST using leaf-set algorithm
// ============================================================================

// Leaf set: tracks current attachment points in the tree
typedef struct
{
    cli_tree_node_t **nodes;
    uint32_t count;
    uint32_t capacity;
} leaf_set_t;

static leaf_set_t *leaf_set_create(void)
{
    leaf_set_t *ls = g_malloc0(sizeof(leaf_set_t));
    ls->capacity = 8;
    ls->nodes = g_malloc0(ls->capacity * sizeof(cli_tree_node_t *));
    return ls;
}

static void leaf_set_add(leaf_set_t *ls, cli_tree_node_t *node)
{
    if (ls->count >= ls->capacity)
    {
        ls->capacity *= 2;
        ls->nodes = realloc(ls->nodes, ls->capacity * sizeof(cli_tree_node_t *));
    }
    ls->nodes[ls->count++] = node;
}

static void leaf_set_free(leaf_set_t *ls)
{
    if (ls)
    {
        g_free(ls->nodes);
        g_free(ls);
    }
}

// Create a tree node from an element
static cli_tree_node_t *create_tree_node_from_element(cli_element_t *element, cli_command_group_t *group,
                                                      uint32_t module_id, uint32_t view_id)
{
    cli_tree_node_t *node =
        cli_tree_create_node(element->cfg_id, element->name, element->description,
                             element->type == ELEMENT_TYPE_KEYWORD ? CLI_NODE_COMMAND : CLI_NODE_ARGUMENT, module_id,
                             group->group_id, view_id);

    if (element->type == ELEMENT_TYPE_PARAMETER && element->param_type)
    {
        cli_param_type_t *param_type_copy = NULL;
        if (element->param_type->type_str)
        {
            param_type_copy = cli_param_type_parse(element->param_type->type_str);
        }
        cli_tree_set_param_type(node, param_type_copy);
    }

    return node;
}

// Helper: add node to leaf set if not already present
static void leaf_set_add_unique(leaf_set_t *ls, cli_tree_node_t *node)
{
    for (uint32_t i = 0; i < ls->count; i++)
    {
        if (ls->nodes[i] == node)
        {
            return;
        }
    }
    leaf_set_add(ls, node);
}

// Forward declaration
static leaf_set_t *build_tree_recursive(expr_node_t *ast, leaf_set_t *parents, cli_command_group_t *group,
                                        uint32_t module_id, uint32_t view_id);

// Build tree from AST node, attaching to parent leaf set.
// Uses a virtual root as anchor so the leaf set is never empty.
// Returns new leaf set (the nodes where next items should attach).
static leaf_set_t *build_tree_recursive(expr_node_t *ast, leaf_set_t *parents, cli_command_group_t *group,
                                        uint32_t module_id, uint32_t view_id)
{
    if (!ast)
    {
        return parents;
    }

    switch (ast->type)
    {
        case EXPR_NODE_ELEMENT:
        {
            cli_element_t *element = cli_group_find_element(group, ast->element_id);
            if (!element)
            {
                return parents;
            }

            // Attach to each parent in the leaf set
            leaf_set_t *new_leaves = leaf_set_create();
            for (uint32_t i = 0; i < parents->count; i++)
            {
                cli_tree_node_t *node = create_tree_node_from_element(element, group, module_id, view_id);
                cli_tree_add_child(parents->nodes[i], node);
                // After add_child, the node might have been merged. Find the actual child.
                cli_tree_node_t *actual = cli_tree_find_child(parents->nodes[i], element->name);
                if (actual)
                {
                    leaf_set_add_unique(new_leaves, actual);
                }
            }
            leaf_set_free(parents);
            return new_leaves;
        }

        case EXPR_NODE_SEQUENCE:
        {
            leaf_set_t *current_leaves = parents;
            for (uint32_t i = 0; i < ast->num_children; i++)
            {
                current_leaves = build_tree_recursive(ast->children[i], current_leaves, group, module_id, view_id);
            }
            return current_leaves;
        }

        case EXPR_NODE_REQUIRED:
        case EXPR_NODE_OPTIONAL:
        {
            // For each alternative, process it starting from the same parent set
            // Collect all resulting leaf sets into one union
            leaf_set_t *result_leaves = leaf_set_create();

            for (uint32_t i = 0; i < ast->num_children; i++)
            {
                // Clone the parent leaf set for each alternative
                leaf_set_t *alt_parents = leaf_set_create();
                for (uint32_t j = 0; j < parents->count; j++)
                {
                    leaf_set_add(alt_parents, parents->nodes[j]);
                }

                leaf_set_t *alt_leaves = build_tree_recursive(ast->children[i], alt_parents, group, module_id, view_id);

                // Merge alt_leaves into result
                for (uint32_t j = 0; j < alt_leaves->count; j++)
                {
                    leaf_set_add_unique(result_leaves, alt_leaves->nodes[j]);
                }
                leaf_set_free(alt_leaves);
            }

            // For OPTIONAL: also include the original parents as leaves (skip path)
            if (ast->type == EXPR_NODE_OPTIONAL)
            {
                for (uint32_t i = 0; i < parents->count; i++)
                {
                    leaf_set_add_unique(result_leaves, parents->nodes[i]);
                }
            }

            leaf_set_free(parents);
            return result_leaves;
        }

        default:
            return parents;
    }
}

// Build command tree from expression string (supports [ ] and { } syntax).
// Returns a virtual root node whose children are the actual command trees.
// The caller should add each child to the target view's cmd_tree, then g_free the virtual root shell.
static cli_tree_node_t *build_tree_from_expression_str(const char *expression, cli_command_group_t *group,
                                                       uint32_t module_id, uint32_t view_id)
{
    if (!expression || !group)
    {
        return NULL;
    }

    // Parse expression into AST
    expr_tokenizer_t tok = {.input = expression, .pos = 0};
    expr_node_t *ast = parse_expr(&tok);
    if (!ast)
    {
        return NULL;
    }

    // Create virtual root as anchor so leaf set is never empty
    cli_tree_node_t *virtual_root = cli_tree_create_node(0, "__virtual_root__", NULL, CLI_NODE_COMMAND, 0, 0, 0);

    leaf_set_t *initial_parents = leaf_set_create();
    leaf_set_add(initial_parents, virtual_root);

    leaf_set_t *final_leaves = build_tree_recursive(ast, initial_parents, group, module_id, view_id);

    // Mark all final leaf nodes as end nodes (skip virtual root itself)
    if (final_leaves)
    {
        for (uint32_t i = 0; i < final_leaves->count; i++)
        {
            if (final_leaves->nodes[i] != virtual_root)
            {
                final_leaves->nodes[i]->is_end_node = TRUE;
            }
        }
        leaf_set_free(final_leaves);
    }

    expr_node_free(ast);

    // If virtual root has no children, nothing was built
    if (virtual_root->num_children == 0)
    {
        cli_tree_free(virtual_root);
        return NULL;
    }

    return virtual_root;
}

// Register command trees (children of virtual root) to a target view's cmd_tree
static void register_cmd_trees_to_view(cli_tree_node_t *virtual_root, cli_tree_node_t *target_cmd_tree, uint32_t clone)
{
    for (uint32_t i = 0; i < virtual_root->num_children; i++)
    {
        cli_tree_node_t *tree = clone ? cli_tree_clone(virtual_root->children[i]) : virtual_root->children[i];
        if (tree)
        {
            cli_tree_add_child(target_cmd_tree, tree);
        }
    }
}

// Parse element definition
static cli_element_t *parse_element(xmlNode *element_node, uint32_t element_id)
{
    xmlChar *cfg_id_str = xmlGetProp(element_node, (const xmlChar *)"cfg-id");
    xmlChar *type_str = xmlGetProp(element_node, (const xmlChar *)"type");
    uint32_t cfg_id = 0;
    element_type_t type = ELEMENT_TYPE_KEYWORD;

    if (cfg_id_str != NULL)
    {
        cfg_id = atoi((const char *)cfg_id_str);
        xmlFree(cfg_id_str);
    }
    if (type_str != NULL)
    {
        type = (strcmp((const char *)type_str, "keyword") == ERRCODE_SUCCESS) ? ELEMENT_TYPE_KEYWORD
                                                                              : ELEMENT_TYPE_PARAMETER;
        xmlFree(type_str);
    }

    // Get name, description, range, type (for parameters)
    char *name = NULL;
    char *description = NULL;
    char *range = NULL;
    char *param_type_str = NULL;

    for (xmlNode *cur = element_node->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"name") == ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            name = g_strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"description") == ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            description = g_strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"range") == ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            range = g_strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"type") == ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            param_type_str = g_strdup((const char *)content);
            xmlFree(content);
        }
    }

    cli_element_t *element = NULL;

    // If parameter type string is provided, use the new constructor
    if (type == ELEMENT_TYPE_PARAMETER && param_type_str)
    {
        element = cli_element_create_with_type(element_id, cfg_id, type, name, description, param_type_str);
    }
    else
    {
        element = cli_element_create(element_id, cfg_id, type, name, description, range);
    }

    g_free(name);
    g_free(description);
    g_free(range);
    g_free(param_type_str);

    return element;
}

// Parse view node recursively
static cli_view_node_t *parse_view_node(xmlNode *view_xml)
{
    char view_name[CLI_CLI_MAX_VIEW_LEN];

    xmlChar *view_id_str = xmlGetProp(view_xml, (const xmlChar *)"view-id");
    if (!view_id_str)
    {
        return NULL;
    }

    uint32_t view_id = atoi((const char *)view_id_str);
    xmlFree(view_id_str);

    xmlChar *view_name_str = xmlGetProp(view_xml, (const xmlChar *)"view-name");
    if (view_name_str != NULL)
    {
        view_name[0] = '\0';
        strlcpy(view_name, (const char *)view_name_str, CLI_CLI_MAX_VIEW_LEN);
        xmlFree(view_name_str);
    }
    // Get template
    char *template = NULL;
    for (xmlNode *cur = view_xml->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        if (xmlStrcmp(cur->name, (const xmlChar *)"template") == ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            template = g_strdup((const char *)content);
            xmlFree(content);
            break;
        }
    }

    // Create view node
    cli_view_node_t *view = cli_view_create(view_id, view_name, template);

    g_free(template);

    if (!view)
    {
        return NULL;
    }

    // Parse child views recursively
    for (xmlNode *cur = view_xml->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        if (xmlStrcmp(cur->name, (const xmlChar *)"view") == ERRCODE_SUCCESS)
        {
            cli_view_node_t *child = parse_view_node(cur);
            if (child)
            {
                cli_view_add_child(view, child);
            }
        }
    }

    return view;
}

// Parse command group and register commands to views
static void parse_command_group(xmlNode *group_node, cli_view_tree_t *view_tree, uint32_t module_id)
{
    uint32_t element_id = 0;

    xmlChar *group_id_str = xmlGetProp(group_node, (const xmlChar *)"group-id");
    if (!group_id_str)
    {
        return;
    }

    cli_command_group_t *group = cli_group_create(atoi((const char *)group_id_str));
    xmlFree(group_id_str);

    if (!group)
    {
        return;
    }

    // Parse elements
    for (xmlNode *cur = group_node->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"elements") == ERRCODE_SUCCESS)
        {
            for (xmlNode *elem = cur->children; elem; elem = elem->next)
            {
                if (elem->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(elem->name, (const xmlChar *)"element") == ERRCODE_SUCCESS)
                {
                    element_id++;
                    cli_element_t *element = parse_element(elem, element_id);
                    if (element)
                    {
                        cli_group_add_element(group, element);
                    }
                }
            }
        }
    }

    // Parse commands and register to views
    for (xmlNode *cur = group_node->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"commands") == ERRCODE_SUCCESS)
        {
            for (xmlNode *cmd = cur->children; cmd; cmd = cmd->next)
            {
                if (cmd->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(cmd->name, (const xmlChar *)"command") == ERRCODE_SUCCESS)
                {
                    char *expression = NULL;
                    char *views = NULL;
                    uint32_t view_id = 0;

                    for (xmlNode *child = cmd->children; child; child = child->next)
                    {
                        if (child->type != XML_ELEMENT_NODE)
                        {
                            continue;
                        }

                        if (xmlStrcmp(child->name, (const xmlChar *)"expression") == ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            g_free(expression);
                            expression = g_strdup((const char *)content);
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(child->name, (const xmlChar *)"views") == ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            g_free(views);
                            views = g_strdup((const char *)content);
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(child->name, (const xmlChar *)"view-id") == ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            view_id = atoi((const char *)content);
                            xmlFree(content);
                        }
                    }

                    if (expression && views)
                    {
                        // Build tree from expression (supports [ ] and { } syntax)
                        cli_tree_node_t *virtual_root =
                            build_tree_from_expression_str(expression, group, module_id, view_id);

                        if (virtual_root)
                        {
                            // Register to specified views
                            if (atoi(views) == CLI_VIEW_GLOBAL)
                            {
                                if (!view_tree->global_view)
                                {
                                    view_tree->global_view = cli_view_create(CLI_VIEW_GLOBAL, "global", NULL);
                                }
                                if (view_tree->global_view && view_tree->global_view->cmd_tree == NULL)
                                {
                                    view_tree->global_view->cmd_tree = cli_tree_create_node(
                                        0, "global_root", "global command root", CLI_NODE_COMMAND, 0, 0, 0);
                                }
                                if (view_tree->global_view)
                                {
                                    register_cmd_trees_to_view(virtual_root, view_tree->global_view->cmd_tree, 0);
                                }
                            }
                            else
                            {
                                char *views_copy = g_strdup(views);
                                char *view_token = strtok(views_copy, ",");
                                uint32_t first = 1;

                                while (view_token)
                                {
                                    while (isspace(*view_token))
                                    {
                                        view_token++;
                                    }
                                    char *end = view_token + strlen(view_token) - 1;
                                    while (end > view_token && isspace(*end))
                                    {
                                        *end-- = '\0';
                                    }

                                    cli_view_node_t *target_view =
                                        cli_view_find_by_id(view_tree->root, atoi(view_token));
                                    if (target_view && target_view->cmd_tree)
                                    {
                                        register_cmd_trees_to_view(virtual_root, target_view->cmd_tree, !first);
                                        first = 0;
                                    }

                                    view_token = strtok(NULL, ",");
                                }

                                g_free(views_copy);
                            }

                            // Free virtual root shell (children already moved)
                            g_free(virtual_root->name);
                            g_free(virtual_root->description);
                            g_free(virtual_root->children);
                            g_free(virtual_root);
                        }
                    }

                    g_free(expression);
                    g_free(views);
                }
            }
        }
    }

    cli_group_free(group);
}

// Load CLI view tree from XML file
uint32_t cli_xml_load_view_tree(const char *xml_file, cli_view_tree_t *view_tree)
{
    if (!xml_file || !view_tree)
    {
        return ERRCODE_FAIL;
    }

    // Parse XML file
    xmlDoc *doc = xmlReadFile(xml_file, NULL, 0);
    if (!doc)
    {
        LOG_ERROR("Could not parse file %s", xml_file);
        return ERRCODE_FAIL;
    }

    // Get root element
    xmlNode *root_element = xmlDocGetRootElement(doc);
    if (!root_element)
    {
        LOG_ERROR("Empty XML document");
        xmlFreeDoc(doc);
        return ERRCODE_FAIL;
    }

    // Extract module name from XML root element
    xmlChar *module_id_str = xmlGetProp(root_element, (const xmlChar *)"module-id");
    if (module_id_str == NULL)
    {
        LOG_ERROR("parse module_id fail");
        xmlFreeDoc(doc);
        return ERRCODE_FAIL;
    }

    uint32_t module_id = atoi((const char *)module_id_str);
    LOG_INFO("Loading XML for module: %u", module_id);
    xmlFree(module_id_str);

    // Parse views section
    for (xmlNode *cur = root_element->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"views") == ERRCODE_SUCCESS)
        {
            for (xmlNode *view_node = cur->children; view_node; view_node = view_node->next)
            {
                if (view_node->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(view_node->name, (const xmlChar *)"view") == ERRCODE_SUCCESS)
                {
                    cli_view_node_t *new_view = parse_view_node(view_node);
                    if (new_view != NULL)
                    {
                        if (view_tree->root == NULL)
                        {
                            view_tree->root = new_view;
                        }
                        else
                        {
                            cli_view_node_t *existing = cli_view_find_by_id(view_tree->root, new_view->view_id);
                            if (existing == NULL)
                            {
                                cli_view_node_t *parent = cli_view_find_by_id(view_tree->root, CLI_VIEW_CONFIG);
                                if (parent == NULL)
                                {
                                    LOG_ERROR("config view does not exist");
                                    continue;
                                }
                                cli_view_add_child(parent, new_view);
                            }
                            else
                            {
                                LOG_ERROR("view %u exist", new_view->view_id);
                            }
                        }
                    }
                }
            }
        }
    }

    // Parse command groups
    for (xmlNode *cur = root_element->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"command_groups") == ERRCODE_SUCCESS)
        {
            for (xmlNode *group_node = cur->children; group_node; group_node = group_node->next)
            {
                if (group_node->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(group_node->name, (const xmlChar *)"group") == ERRCODE_SUCCESS)
                {
                    parse_command_group(group_node, view_tree, module_id);
                }
            }
        }
    }

    // Merge global commands into all views
    if (view_tree->global_view && view_tree->global_view->cmd_tree)
    {
        merge_global_to_views(view_tree->root, view_tree->global_view->cmd_tree);
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();

    return ERRCODE_SUCCESS;
}

// Helper to merge global commands into all views
static void merge_global_to_views(cli_view_node_t *view, cli_tree_node_t *global_tree)
{
    if (!view || !global_tree)
    {
        return;
    }

    // Clone global commands into this view
    for (uint32_t i = 0; i < global_tree->num_children; i++)
    {
        cli_tree_node_t *global_cmd = cli_tree_clone(global_tree->children[i]);
        if (global_cmd && view->cmd_tree)
        {
            cli_tree_add_child(view->cmd_tree, global_cmd);
        }
    }

    // Recursively merge into child views
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        merge_global_to_views(view->children[i], global_tree);
    }
}
