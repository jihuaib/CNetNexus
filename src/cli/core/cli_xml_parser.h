/**
 * @file   cli_xml_parser.h
 * @brief  CLI XML 配置文件解析头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_XML_PARSER_H
#define CLI_XML_PARSER_H

#include <glib.h>

#include "cli_tree.h"
#include "cli_view.h"

// ============================================================================
// Intermediate Database Structures (for XML parsing)
// ============================================================================

typedef struct cfg_xml_db_field
{
    char *field_name;
    char *type_str;
} cfg_xml_db_field_t;

typedef struct cfg_xml_db_table
{
    char *table_name;
    GList *fields; // List of cfg_xml_db_field_t*
} cfg_xml_db_table_t;

typedef struct cfg_xml_db_def
{
    char *db_name;
    uint32_t module_id;
    GList *tables; // List of cfg_xml_db_table_t*
} cfg_xml_db_def_t;

// Load CLI view tree from XML file
uint32_t cli_xml_load_view_tree(const char *xml_file, cli_view_tree_t *view_tree);

// Optional: set pointer to list used to collect <dbs> definitions (NULL to ignore)
void cli_xml_set_db_def_list(GList **list);

// Cleanup functions for intermediate structures
void cfg_xml_db_def_free(cfg_xml_db_def_t *db_def);

#endif // CLI_XML_PARSER_H
