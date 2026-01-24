#ifndef NN_CFG_H
#define NN_CFG_H

#include <stdint.h>

// Register a module's XML configuration path by module ID
// This should be called by modules in their constructor after nn_dev_register_module
void nn_cfg_register_module_xml(uint32_t module_id, const char *xml_path);

#endif // NN_CFG_H
