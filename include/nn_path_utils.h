#ifndef NN_PATH_UTILS_H
#define NN_PATH_UTILS_H

#include <stddef.h>

/**
 * @brief Get the directory containing the executable
 * @param buf Buffer to store the directory path
 * @param size Size of the buffer
 * @return 0 on success, -1 on failure
 */
int nn_get_exe_dir(char *buf, size_t size);

/**
 * @brief Resolve XML file path for a module
 * @param module_name Module name (e.g., "bgp", "dev", "cfg")
 * @param buf Buffer to store the resolved path
 * @param size Size of the buffer
 * @return 0 on success, -1 on failure
 *
 * This function resolves XML paths in the following order:
 * 1. Environment variable NETNEXUS_XML_DIR if set
 * 2. Relative to executable directory (for development builds)
 * 3. System installation path (for production)
 */
int nn_resolve_xml_path(const char *module_name, char *buf, size_t size);

#endif // NN_PATH_UTILS_H
