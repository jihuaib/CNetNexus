#include "nn_cli_xml_parser.h"

#include <ctype.h>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_cli_element.h"
#include "nn_cli_tree.h"
#include "nn_cli_view.h"
#include "nn_errcode.h"

// Forward declaration of command callbacks
extern void cmd_help(uint32_t client_fd, const char *args);
extern void cmd_exit(uint32_t client_fd, const char *args);
extern void cmd_show_version(uint32_t client_fd, const char *args);
extern void cmd_show_tree(uint32_t client_fd, const char *args);
extern void cmd_configure(uint32_t client_fd, const char *args);
extern void cmd_end(uint32_t client_fd, const char *args);
extern void cmd_sysname(uint32_t client_fd, const char *args);

// Map callback name to function pointer
static nn_cli_callback_t get_callback_by_name(const char *name)
{
    if (!name)
    {
        return NULL;
    }

    if (strcmp(name, "cmd_help") == NN_ERRCODE_SUCCESS)
    {
        return cmd_help;
    }
    if (strcmp(name, "cmd_exit") == NN_ERRCODE_SUCCESS)
    {
        return cmd_exit;
    }
    if (strcmp(name, "cmd_show_version") == NN_ERRCODE_SUCCESS)
    {
        return cmd_show_version;
    }
    if (strcmp(name, "cmd_show_tree") == NN_ERRCODE_SUCCESS)
    {
        return cmd_show_tree;
    }
    if (strcmp(name, "cmd_configure") == NN_ERRCODE_SUCCESS)
    {
        return cmd_configure;
    }
    if (strcmp(name, "cmd_end") == NN_ERRCODE_SUCCESS)
    {
        return cmd_end;
    }
    if (strcmp(name, "cmd_sysname") == NN_ERRCODE_SUCCESS)
    {
        return cmd_sysname;
    }

    return NULL;
}

// Forward declarations
static void merge_global_to_views(nn_cli_view_node_t *view, nn_cli_tree_node_t *global_tree);

// Parse expression string "1 2 3" into array of IDs
static uint32_t *parse_expression(const char *expr, uint32_t *count)
{
    if (!expr || !count)
    {
        return NULL;
    }

    *count = 0;
    uint32_t capacity = 10;
    uint32_t *ids = (uint32_t *)g_malloc(capacity * sizeof(uint32_t));

    char *expr_copy = strdup(expr);
    char *token = strtok(expr_copy, " \t\n");

    while (token)
    {
        if (*count >= capacity)
        {
            capacity *= 2;
            uint32_t *new_ids = (uint32_t *)realloc(ids, capacity * sizeof(uint32_t));
            if (!new_ids)
            {
                g_free(ids);
                g_free(expr_copy);
                return NULL;
            }
            ids = new_ids;
        }

        ids[(*count)++] = atoi(token);
        token = strtok(NULL, " \t\n");
    }

    g_free(expr_copy);
    return ids;
}

// Build tree from expression (element IDs)
static nn_cli_tree_node_t *build_tree_from_expression(uint32_t *element_ids, uint32_t count,
                                                      nn_cli_command_group_t *group)
{
    if (!element_ids || count == NN_ERRCODE_SUCCESS || !group)
    {
        return NULL;
    }

    nn_cli_tree_node_t *root = NULL;
    nn_cli_tree_node_t *current = NULL;

    for (uint32_t i = 0; i < count; i++)
    {
        nn_cli_element_t *element = nn_cli_group_find_element(group, element_ids[i]);
        if (!element)
        {
            continue;
        }

        nn_cli_tree_node_t *node =
            nn_cli_tree_create_node(element->name, element->description,
                                    element->type == ELEMENT_TYPE_KEYWORD ? NN_CLI_NODE_COMMAND : NN_CLI_NODE_ARGUMENT);

        // Set param_type for ARGUMENT nodes
        if (element->type == ELEMENT_TYPE_PARAMETER && element->param_type)
        {
            // Clone the param_type from element to node
            nn_cli_param_type_t *param_type_copy = NULL;
            if (element->param_type->type_str)
            {
                param_type_copy = nn_cli_param_type_parse(element->param_type->type_str);
            }
            nn_cli_tree_set_param_type(node, param_type_copy);
        }

        if (!root)
        {
            root = node;
            current = node;
        }
        else
        {
            nn_cli_tree_add_child(current, node);
            current = node;
        }
    }

    return root;
}

// Parse element definition
static nn_cli_element_t *parse_element(xmlNode *element_node)
{
    xmlChar *id_str = xmlGetProp(element_node, (const xmlChar *)"id");
    xmlChar *type_str = xmlGetProp(element_node, (const xmlChar *)"type");

    if (!id_str || !type_str)
    {
        if (id_str)
        {
            xmlFree(id_str);
        }
        if (type_str)
        {
            xmlFree(type_str);
        }
        return NULL;
    }

    uint32_t id = atoi((const char *)id_str);
    element_type_t type = (strcmp((const char *)type_str, "keyword") == NN_ERRCODE_SUCCESS) ? ELEMENT_TYPE_KEYWORD
                                                                                            : ELEMENT_TYPE_PARAMETER;

    xmlFree(id_str);
    xmlFree(type_str);

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

        if (xmlStrcmp(cur->name, (const xmlChar *)"name") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            name = strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"description") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            description = strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"range") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            range = strdup((const char *)content);
            xmlFree(content);
        }
        else if (xmlStrcmp(cur->name, (const xmlChar *)"type") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            param_type_str = strdup((const char *)content);
            xmlFree(content);
        }
    }

    nn_cli_element_t *element = NULL;

    // If parameter type string is provided, use the new constructor
    if (type == ELEMENT_TYPE_PARAMETER && param_type_str)
    {
        element = nn_cli_element_create_with_type(id, type, name, description, param_type_str);
    }
    else
    {
        element = nn_cli_element_create(id, type, name, description, range);
    }

    g_free(name);
    g_free(description);
    g_free(range);
    g_free(param_type_str);

    return element;
}

// Parse view node recursively
static nn_cli_view_node_t *parse_view_node(xmlNode *view_xml)
{
    xmlChar *view_name = xmlGetProp(view_xml, (const xmlChar *)"name");
    if (!view_name)
    {
        return NULL;
    }

    // Get template
    char *template = NULL;
    for (xmlNode *cur = view_xml->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        if (xmlStrcmp(cur->name, (const xmlChar *)"template") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            template = strdup((const char *)content);
            xmlFree(content);
            break;
        }
    }

    // Create view node
    nn_cli_view_node_t *view = nn_cli_view_create((const char *)view_name, template);
    xmlFree(view_name);
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
        if (xmlStrcmp(cur->name, (const xmlChar *)"view") == NN_ERRCODE_SUCCESS)
        {
            nn_cli_view_node_t *child = parse_view_node(cur);
            if (child)
            {
                nn_cli_view_add_child(view, child);
            }
        }
    }

    return view;
}

// Parse command group and register commands to views
static void parse_command_group(xmlNode *group_node, nn_cli_view_tree_t *view_tree, const char *module_name)
{
    xmlChar *group_name = xmlGetProp(group_node, (const xmlChar *)"name");
    if (!group_name)
    {
        return;
    }

    nn_cli_command_group_t *group = nn_cli_group_create((const char *)group_name);
    xmlFree(group_name);

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

        if (xmlStrcmp(cur->name, (const xmlChar *)"elements") == NN_ERRCODE_SUCCESS)
        {
            for (xmlNode *elem = cur->children; elem; elem = elem->next)
            {
                if (elem->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(elem->name, (const xmlChar *)"element") == NN_ERRCODE_SUCCESS)
                {
                    nn_cli_element_t *element = parse_element(elem);
                    if (element)
                    {
                        nn_cli_group_add_element(group, element);
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

        if (xmlStrcmp(cur->name, (const xmlChar *)"commands") == NN_ERRCODE_SUCCESS)
        {
            for (xmlNode *cmd = cur->children; cmd; cmd = cmd->next)
            {
                if (cmd->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(cmd->name, (const xmlChar *)"command") == NN_ERRCODE_SUCCESS)
                {
                    char *expression = NULL;
                    char *views = NULL;
                    char *callback_name = NULL;

                    for (xmlNode *child = cmd->children; child; child = child->next)
                    {
                        if (child->type != XML_ELEMENT_NODE)
                        {
                            continue;
                        }

                        if (xmlStrcmp(child->name, (const xmlChar *)"expression") == NN_ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            g_free(expression); // Free previous allocation if any
                            expression = strdup((const char *)content);
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(child->name, (const xmlChar *)"views") == NN_ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            g_free(views); // Free previous allocation if any
                            views = strdup((const char *)content);
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(child->name, (const xmlChar *)"callback") == NN_ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            g_free(callback_name); // Free previous allocation if any
                            callback_name = strdup((const char *)content);
                            xmlFree(content);
                        }
                    }

                    if (expression && views)
                    {
                        // Parse expression into element IDs
                        uint32_t count = 0;
                        uint32_t *element_ids = parse_expression(expression, &count);

                        if (element_ids && count > 0)
                        {
                            // Build tree from expression
                            nn_cli_tree_node_t *cmd_tree = build_tree_from_expression(element_ids, count, group);

                            if (cmd_tree && callback_name)
                            {
                                // Set callback on leaf node
                                nn_cli_tree_node_t *leaf = cmd_tree;
                                while (leaf->num_children > 0)
                                {
                                    leaf = leaf->children[0];
                                }
                                nn_cli_callback_t callback = get_callback_by_name(callback_name);
                                if (callback)
                                {
                                    nn_cli_tree_set_callback(leaf, callback);
                                }

                                // Set module name for message dispatch
                                if (module_name)
                                {
                                    nn_cli_tree_set_module_name(leaf, module_name);
                                    printf("[xml_parser] Command registered to module '%s'\n", module_name);
                                }
                            }

                            // Register to specified views
                            if (strcmp(views, "global") == NN_ERRCODE_SUCCESS)
                            {
                                // Add to global view
                                if (!view_tree->global_view)
                                {
                                    view_tree->global_view = nn_cli_view_create("global", NULL);
                                }
                                if (view_tree->global_view && cmd_tree)
                                {
                                    nn_cli_tree_add_child(view_tree->global_view->cmd_tree, cmd_tree);
                                }
                            }
                            else
                            {
                                // Add to specific views
                                char *views_copy = strdup(views);
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

                                    nn_cli_view_node_t *target_view =
                                        nn_cli_view_find_by_name(view_tree->root, view_token);
                                    if (target_view && target_view->cmd_tree)
                                    {
                                        nn_cli_tree_node_t *tree_to_add =
                                            first ? cmd_tree : nn_cli_tree_clone(cmd_tree);
                                        if (tree_to_add)
                                        {
                                            nn_cli_tree_add_child(target_view->cmd_tree, tree_to_add);
                                        }
                                        first = 0;
                                    }

                                    view_token = strtok(NULL, ",");
                                }

                                g_free(views_copy);
                            }
                        }

                        g_free(element_ids);
                    }

                    g_free(expression);
                    g_free(views);
                    g_free(callback_name);
                }
            }
        }
    }

    nn_cli_group_free(group);
}

// Load CLI view tree from XML file
uint32_t nn_cli_xml_load_view_tree(const char *xml_file, nn_cli_view_tree_t *view_tree)
{
    if (!xml_file || !view_tree)
    {
        return NN_ERRCODE_FAIL;
    }

    // Parse XML file
    xmlDoc *doc = xmlReadFile(xml_file, NULL, 0);
    if (!doc)
    {
        fprintf(stderr, "Error: Could not parse file %s\n", xml_file);
        return NN_ERRCODE_FAIL;
    }

    // Get root element
    xmlNode *root_element = xmlDocGetRootElement(doc);
    if (!root_element)
    {
        fprintf(stderr, "Error: Empty XML document\n");
        xmlFreeDoc(doc);
        return NN_ERRCODE_FAIL;
    }

    // Extract module name from XML root element
    const char *module_name = NULL;
    xmlChar *mod_attr = xmlGetProp(root_element, (const xmlChar *)"module_id");
    if (mod_attr)
    {
        module_name = strdup((const char *)mod_attr);
        xmlFree(mod_attr);
    }

    if (module_name)
    {
        printf("[xml_parser] Loading XML for module: %s\n", module_name);
    }
    else
    {
        printf("[xml_parser] Loading XML (no module associated)\n");
    }

    // Parse views section
    for (xmlNode *cur = root_element->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"views") == NN_ERRCODE_SUCCESS)
        {
            for (xmlNode *view_node = cur->children; view_node; view_node = view_node->next)
            {
                if (view_node->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(view_node->name, (const xmlChar *)"view") == NN_ERRCODE_SUCCESS)
                {
                    nn_cli_view_node_t *new_view = parse_view_node(view_node);
                    if (new_view)
                    {
                        if (view_tree->root == NULL)
                        {
                            view_tree->root = new_view;
                        }
                        else
                        {
                            // Check if view with same name already exists
                            nn_cli_view_node_t *existing = nn_cli_view_find_by_name(view_tree->root, new_view->name);
                            if (existing)
                            {
                                // If it exists, we just merge children.
                                // For simplicity now, we just g_free the new shell and use existing.
                                // But parse_view_node already created a tree.
                                // We should probably have a dedicated merge function.
                                // For now, let's just add the children of the new root to the existing one.
                                for (uint32_t i = 0; i < new_view->num_children; i++)
                                {
                                    nn_cli_view_add_child(existing, new_view->children[i]);
                                }
                                // We need to be careful with memory here, but let's assume simple cases.
                                // Clean up the new shell (not recursive since children were moved)
                                g_free(new_view->name);
                                g_free(new_view->prompt_template);
                                g_free(new_view->children);
                                g_free(new_view);
                            }
                            else
                            {
                                // New view. Find appropriate parent.
                                // If it's a module XML (module_name != NULL), try to add under "config"
                                nn_cli_view_node_t *parent = NULL;
                                if (module_name)
                                {
                                    parent = nn_cli_view_find_by_name(view_tree->root, "config");
                                }

                                if (!parent)
                                {
                                    parent = view_tree->root;
                                }

                                nn_cli_view_add_child(parent, new_view);
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

        if (xmlStrcmp(cur->name, (const xmlChar *)"command_groups") == NN_ERRCODE_SUCCESS)
        {
            for (xmlNode *group_node = cur->children; group_node; group_node = group_node->next)
            {
                if (group_node->type != XML_ELEMENT_NODE)
                {
                    continue;
                }

                if (xmlStrcmp(group_node->name, (const xmlChar *)"group") == NN_ERRCODE_SUCCESS)
                {
                    parse_command_group(group_node, view_tree, module_name);
                }
            }
        }
    }

    // Merge global commands into all views
    if (view_tree->global_view && view_tree->global_view->cmd_tree)
    {
        merge_global_to_views(view_tree->root, view_tree->global_view->cmd_tree);
    }

    // Cleanup
    if (module_name)
    {
        g_free((void *)module_name);
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();

    return NN_ERRCODE_SUCCESS;
}

// Helper to merge global commands into all views
static void merge_global_to_views(nn_cli_view_node_t *view, nn_cli_tree_node_t *global_tree)
{
    if (!view || !global_tree)
    {
        return;
    }

    // Clone global commands into this view
    for (uint32_t i = 0; i < global_tree->num_children; i++)
    {
        nn_cli_tree_node_t *global_cmd = nn_cli_tree_clone(global_tree->children[i]);
        if (global_cmd && view->cmd_tree)
        {
            nn_cli_tree_add_child(view->cmd_tree, global_cmd);
        }
    }

    // Recursively merge into child views
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        merge_global_to_views(view->children[i], global_tree);
    }
}
