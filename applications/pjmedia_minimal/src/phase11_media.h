#ifndef PHASE11_MEDIA_H
#define PHASE11_MEDIA_H

#include <pjlib.h>
#include <pjsip.h>
#include <pjsip-ua/sip_inv.h>

pj_status_t phase11_media_lifecycle_init(pj_pool_factory *factory,
					 pjsip_endpoint *sip_endpt);
pj_status_t phase11_media_prepare_call(void);
unsigned phase11_media_sdp_port(pj_bool_t uas);
pj_status_t phase11_media_start_call(pjsip_inv_session *uac,
				     pjsip_inv_session *uas);
pj_status_t phase11_media_exercise_call(void);
pj_status_t phase11_media_stop_call(void);
pj_status_t phase11_media_lifecycle_destroy(void);

#endif
