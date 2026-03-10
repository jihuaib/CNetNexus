/**
 * @file   cli_xml_parser.h
 * @brief  CLI XML 配置文件解析头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_XML_PARSER_H
#define CLI_XML_PARSER_H

#include "cli_tree.h"
#include "cli_view.h"

/**
 * @brief 从 XML 文件加载 CLI 视图树
 * @param xml_file XML 文件路径
 * @param view_tree 视图树
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
uint32_t cli_xml_load_view_tree(const char *xml_file, cli_view_tree_t *view_tree);

#endif // CLI_XML_PARSER_H
