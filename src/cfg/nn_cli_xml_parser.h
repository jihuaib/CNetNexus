#ifndef NN_CLI_XML_PARSER_H
#define NN_CLI_XML_PARSER_H

#include <glib.h>
#include "nn_cli_tree.h"
#include "nn_cli_view.h"

// ============================================================================
// Intermediate Database Structures (for XML parsing)
// ============================================================================

typedef struct nn_cfg_xml_db_field
{
    char *field_name;
    char *type_str;
} nn_cfg_xml_db_field_t;

typedef struct nn_cfg_xml_db_table
{
    char *table_name;
    GList *fields; // List of nn_cfg_xml_db_field_t*
} nn_cfg_xml_db_table_t;

typedef struct nn_cfg_xml_db_def
{
    char *db_name;
    uint32_t module_id;
    GList *tables; // List of nn_cfg_xml_db_table_t*
} nn_cfg_xml_db_def_t;

// Load CLI view tree from XML file
uint32_t nn_cli_xml_load_view_tree(const char *xml_file, nn_cli_view_tree_t *view_tree);

// Cleanup functions for intermediate structures
void nn_cfg_xml_db_def_free(nn_cfg_xml_db_def_t *db_def);

#endif // NN_CLI_XML_PARSER_H
