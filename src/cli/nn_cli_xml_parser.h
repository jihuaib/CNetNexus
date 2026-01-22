#ifndef NN_CLI_XML_PARSER_H
#define NN_CLI_XML_PARSER_H

#include "nn_cli_tree.h"
#include "nn_cli_view.h"

// Load CLI view tree from XML file
uint32_t nn_cli_xml_load_view_tree(const char *xml_file, nn_cli_view_tree_t *view_tree);

// Load commands from XML and register to existing view tree
uint32_t nn_cli_xml_load_commands(const char *xml_file, nn_cli_view_tree_t *view_tree);

#endif // NN_CLI_XML_PARSER_H
