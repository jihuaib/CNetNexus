/**
 * @file   ospf_spf.h
 * @brief  OSPFv2 intra-area SPF and route synchronization
 */
#ifndef OSPF_SPF_H
#define OSPF_SPF_H

#include "ospf_worker.h"

void ospf_spf_recalculate(ospf_instance_t *inst);
void ospf_spf_recalculate_all(void);

#endif /* OSPF_SPF_H */
