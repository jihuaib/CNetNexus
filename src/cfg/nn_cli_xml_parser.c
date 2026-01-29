#include "nn_cli_xml_parser.h"

#include <ctype.h>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../db/nn_db_registry.h"
#include "nn_cfg_main.h"
#include "nn_cli_element.h"
#include "nn_cli_tree.h"
#include "nn_cli_view.h"
#include "nn_errcode.h"

static void merge_global_to_views(nn_cli_view_node_t *view, nn_cli_tree_node_t *global_tree);

// Database intermediate parsing (private)
static nn_cfg_xml_db_def_t *parse_databases_node(xmlNode *dbs_node, uint32_t module_id);
static nn_cfg_xml_db_table_t *parse_table_node(xmlNode *table_node);
static nn_cfg_xml_db_field_t *parse_field_node(xmlNode *field_node);

// Parse expression string "1 2 3" into array of IDs
static uint32_t *parse_expression(const char *expr, uint32_t *count)
{
    if (!expr || !count)
    {
        return NULL;
    }

    *count = 0;
    uint32_t capacity = 10;
    uint32_t *ids = (uint32_t *)g_malloc0(capacity * sizeof(uint32_t));

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
                                                      nn_cli_command_group_t *group, uint32_t module_id,
                                                      uint32_t view_id)
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
            nn_cli_tree_create_node(element->cfg_id, element->name, element->description,
                                    element->type == ELEMENT_TYPE_KEYWORD ? NN_CLI_NODE_COMMAND : NN_CLI_NODE_ARGUMENT,
                                    module_id, group->group_id, view_id);

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

    // Mark the last node as an end node (valid command completion point)
    if (current)
    {
        current->is_end_node = TRUE;
    }

    return root;
}

// Parse element definition
static nn_cli_element_t *parse_element(xmlNode *element_node, uint32_t element_id)
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
        type = (strcmp((const char *)type_str, "keyword") == NN_ERRCODE_SUCCESS) ? ELEMENT_TYPE_KEYWORD
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
        element = nn_cli_element_create_with_type(element_id, cfg_id, type, name, description, param_type_str);
    }
    else
    {
        element = nn_cli_element_create(element_id, cfg_id, type, name, description, range);
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
    char view_name[NN_CFG_CLI_MAX_VIEW_NAME_LEN];

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
        strlcpy(view_name, (const char *)view_name_str, NN_CFG_CLI_MAX_VIEW_NAME_LEN);
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
        if (xmlStrcmp(cur->name, (const xmlChar *)"template") == NN_ERRCODE_SUCCESS)
        {
            xmlChar *content = xmlNodeGetContent(cur);
            template = strdup((const char *)content);
            xmlFree(content);
            break;
        }
    }

    // Create view node
    nn_cli_view_node_t *view = nn_cli_view_create(view_id, view_name, template);

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
static void parse_command_group(xmlNode *group_node, nn_cli_view_tree_t *view_tree, uint32_t module_id)
{
    uint32_t element_id = 0;

    xmlChar *group_id_str = xmlGetProp(group_node, (const xmlChar *)"group-id");
    if (!group_id_str)
    {
        return;
    }

    nn_cli_command_group_t *group = nn_cli_group_create(atoi((const char *)group_id_str));
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
                    element_id++;
                    nn_cli_element_t *element = parse_element(elem, element_id);
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
                    uint32_t view_id = 0;

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
                        else if (xmlStrcmp(child->name, (const xmlChar *)"view-id") == NN_ERRCODE_SUCCESS)
                        {
                            xmlChar *content = xmlNodeGetContent(child);
                            view_id = atoi((const char *)content);
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
                            nn_cli_tree_node_t *cmd_tree =
                                build_tree_from_expression(element_ids, count, group, module_id, view_id);

                            if (cmd_tree)
                            {
                                nn_cli_tree_node_t *leaf = cmd_tree;
                                while (leaf->num_children > 0)
                                {
                                    leaf = leaf->children[0];
                                }
                            }

                            // Register to specified views
                            if (atoi(views) == NN_CFG_CLI_VIEW_GLOBAL)
                            {
                                // Add to global view
                                if (!view_tree->global_view)
                                {
                                    view_tree->global_view = nn_cli_view_create(NN_CFG_CLI_VIEW_GLOBAL, "global", NULL);
                                }
                                if (view_tree->global_view && view_tree->global_view->cmd_tree == NULL)
                                {
                                    view_tree->global_view->cmd_tree = nn_cli_tree_create_node(
                                        0, "global_root", "global command root", NN_CLI_NODE_COMMAND, 0, 0, 0);
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
                                        nn_cli_view_find_by_id(view_tree->root, atoi(view_token));
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
        fprintf(stderr, "[xml_parser] Error: Could not parse file %s\n", xml_file);
        return NN_ERRCODE_FAIL;
    }

    // Get root element
    xmlNode *root_element = xmlDocGetRootElement(doc);
    if (!root_element)
    {
        fprintf(stderr, "[xml_parser] Error: Empty XML document\n");
        xmlFreeDoc(doc);
        return NN_ERRCODE_FAIL;
    }

    // Extract module name from XML root element
    xmlChar *module_id_str = xmlGetProp(root_element, (const xmlChar *)"module-id");
    if (module_id_str == NULL)
    {
        fprintf(stderr, "[xml_parser] Error: parse module_id fail\n");
        xmlFreeDoc(doc);
        return NN_ERRCODE_FAIL;
    }

    uint32_t module_id = atoi((const char *)module_id_str);
    printf("[xml_parser] Loading XML for module: %u\n", module_id);

    xmlFree(module_id_str);

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
                    if (new_view != NULL)
                    {
                        if (view_tree->root == NULL)
                        {
                            view_tree->root = new_view;
                        }
                        else
                        {
                            // Check if view with same name already exists
                            nn_cli_view_node_t *existing = nn_cli_view_find_by_id(view_tree->root, new_view->view_id);
                            if (existing == NULL)
                            {
                                nn_cli_view_node_t *parent =
                                    nn_cli_view_find_by_id(view_tree->root, NN_CFG_CLI_VIEW_CONFIG);
                                if (parent == NULL)
                                {
                                    fprintf(stderr, "[xml_parser] Error: config view does not exist\n");
                                    continue;
                                }
                                nn_cli_view_add_child(parent, new_view);
                            }
                            else
                            {
                                fprintf(stderr, "[xml_parser] Error: view %u exist\n", new_view->view_id);
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
                    parse_command_group(group_node, view_tree, module_id);
                }
            }
        }
    }

    // Parse databases section
    for (xmlNode *cur = root_element->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
        {
            continue;
        }

        if (xmlStrcmp(cur->name, (const xmlChar *)"dbs") == 0)
        {
            nn_cfg_xml_db_def_t *xml_db_def = parse_databases_node(cur, module_id);
            if (xml_db_def && g_nn_cfg_local)
            {
                g_nn_cfg_local->xml_db_defs = g_list_append(g_nn_cfg_local->xml_db_defs, xml_db_def);
            }
            else if (xml_db_def)
            {
                nn_cfg_xml_db_def_free(xml_db_def);
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

// ============================================================================
// Database Definition Parsing Functions (to intermediate structures)
// ============================================================================

static nn_cfg_xml_db_field_t *parse_field_node(xmlNode *field_node)
{
    xmlChar *field_name = xmlGetProp(field_node, (const xmlChar *)"field-name");
    xmlChar *type_str = xmlGetProp(field_node, (const xmlChar *)"type");

    if (!field_name || !type_str)
    {
        if (field_name) xmlFree(field_name);
        if (type_str) xmlFree(type_str);
        return NULL;
    }

    nn_cfg_xml_db_field_t *field = g_malloc0(sizeof(nn_cfg_xml_db_field_t));
    field->field_name = g_strdup((const char *)field_name);
    field->type_str = g_strdup((const char *)type_str);

    xmlFree(field_name);
    xmlFree(type_str);

    return field;
}

static nn_cfg_xml_db_table_t *parse_table_node(xmlNode *table_node)
{
    xmlChar *table_name = xmlGetProp(table_node, (const xmlChar *)"table-name");
    if (!table_name)
    {
        return NULL;
    }

    nn_cfg_xml_db_table_t *table = g_malloc0(sizeof(nn_cfg_xml_db_table_t));
    table->table_name = g_strdup((const char *)table_name);
    xmlFree(table_name);

    for (xmlNode *cur = table_node->children; cur; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE || xmlStrcmp(cur->name, (const xmlChar *)"fields") != 0)
        {
            continue;
        }

        for (xmlNode *field_node = cur->children; field_node; field_node = field_node->next)
        {
            if (field_node->type == XML_ELEMENT_NODE && xmlStrcmp(field_node->name, (const xmlChar *)"field") == 0)
            {
                nn_cfg_xml_db_field_t *field = parse_field_node(field_node);
                if (field)
                {
                    table->fields = g_list_append(table->fields, field);
                }
            }
        }
    }

    return table;
}

static nn_cfg_xml_db_def_t *parse_databases_node(xmlNode *dbs_node, uint32_t module_id)
{
    for (xmlNode *db_node = dbs_node->children; db_node; db_node = db_node->next)
    {
        if (db_node->type != XML_ELEMENT_NODE || xmlStrcmp(db_node->name, (const xmlChar *)"db") != 0)
        {
            continue;
        }

        xmlChar *db_name = xmlGetProp(db_node, (const xmlChar *)"db-name");
        if (!db_name)
        {
            continue;
        }

        nn_cfg_xml_db_def_t *db_def = g_malloc0(sizeof(nn_cfg_xml_db_def_t));
        db_def->db_name = g_strdup((const char *)db_name);
        db_def->module_id = module_id;
        xmlFree(db_name);

        for (xmlNode *cur = db_node->children; cur; cur = cur->next)
        {
            if (cur->type == XML_ELEMENT_NODE && xmlStrcmp(cur->name, (const xmlChar *)"tables") == 0)
            {
                for (xmlNode *table_node = cur->children; table_node; table_node = table_node->next)
                {
                    if (table_node->type == XML_ELEMENT_NODE && xmlStrcmp(table_node->name, (const xmlChar *)"table") == 0)
                    {
                        nn_cfg_xml_db_table_t *table = parse_table_node(table_node);
                        if (table)
                        {
                            db_def->tables = g_list_append(db_def->tables, table);
                        }
                    }
                }
            }
        }
        return db_def;
    }
    return NULL;
}

static void nn_cfg_xml_db_field_free(nn_cfg_xml_db_field_t *field)
{
    if (field)
    {
        g_free(field->field_name);
        g_free(field->type_str);
        g_free(field);
    }
}

static void nn_cfg_xml_db_table_free(nn_cfg_xml_db_table_t *table)
{
    if (table)
    {
        g_free(table->table_name);
        g_list_free_full(table->fields, (GDestroyNotify)nn_cfg_xml_db_field_free);
        g_free(table);
    }
}

void nn_cfg_xml_db_def_free(nn_cfg_xml_db_def_t *db_def)
{
    if (db_def)
    {
        g_free(db_def->db_name);
        g_list_free_full(db_def->tables, (GDestroyNotify)nn_cfg_xml_db_table_free);
        g_free(db_def);
    }
}
