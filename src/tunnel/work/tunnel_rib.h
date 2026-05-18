#ifndef TUNNEL_RIB_H
#define TUNNEL_RIB_H

#include <glib.h>

#include "dev.h"
#include "tunnel.h"

typedef struct tunnel_rib tunnel_rib_t;

typedef enum tunnel_show_section
{
    TUNNEL_SHOW_SUMMARY = 0,
    TUNNEL_SHOW_CANDIDATE,
    TUNNEL_SHOW_NHLFE,
    TUNNEL_SHOW_FTN,
    TUNNEL_SHOW_ILM,
    TUNNEL_SHOW_WATCH,
} tunnel_show_section_t;

tunnel_rib_t *tunnel_rib_create(void);
void tunnel_rib_destroy(tunnel_rib_t *rib);

int tunnel_rib_candidate_add(tunnel_rib_t *rib, const tunnel_candidate_t *candidate);
int tunnel_rib_candidate_del(tunnel_rib_t *rib, const tunnel_candidate_t *candidate);
int tunnel_rib_label_alloc(tunnel_rib_t *rib, const tunnel_label_req_t *req, uint32_t *label_out);
int tunnel_rib_label_release(tunnel_rib_t *rib, const tunnel_label_req_t *req);
int tunnel_rib_watch_add(tunnel_rib_t *rib, uint32_t owner_module_id, const tunnel_resolve_req_t *req);
int tunnel_rib_watch_del(tunnel_rib_t *rib, uint32_t owner_module_id, const tunnel_resolve_req_t *req);
int tunnel_rib_resolve_query(tunnel_rib_t *rib, const tunnel_resolve_req_t *req, tunnel_resolve_notify_t *notify_out);
void tunnel_rib_recompute(tunnel_rib_t *rib);
char *tunnel_rib_show(const tunnel_rib_t *rib, tunnel_show_section_t section);
char *tunnel_rib_show_labels(const tunnel_rib_t *rib);

#endif /* TUNNEL_RIB_H */
