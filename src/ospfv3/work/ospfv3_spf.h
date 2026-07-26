/**
 * @file   ospfv3_spf.h
 * @brief  OSPFv3 intra-area SPF and route synchronization
 */
#ifndef OSPFV3_SPF_H
#define OSPFV3_SPF_H

#include "ospfv3_worker.h"

void ospfv3_spf_recalculate(ospfv3_instance_t *inst);
void ospfv3_spf_recalculate_all(void);

#endif /* OSPFV3_SPF_H */
