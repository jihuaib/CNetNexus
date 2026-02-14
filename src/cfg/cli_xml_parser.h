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

// Load CLI view tree from XML file
uint32_t cli_xml_load_view_tree(const char *xml_file, cli_view_tree_t *view_tree);

#endif // CLI_XML_PARSER_H
