/* Active PJLIB feature configuration for the Zephyr port. */
#ifndef __PJ_CONFIG_SITE_H__
#define __PJ_CONFIG_SITE_H__

#if (defined(PJ_ZEPHYR) && PJ_ZEPHYR != 0) || defined(__ZEPHYR__)

/* Keep optional facilities outside the initial PJLIB-only scope disabled. */
#define PJ_HAS_FLOATING_POINT               0
#define PJ_HAS_SSL_SOCK                     0

/* These implementations correspond to the explicit Zephyr source set. */
#define PJ_IOQUEUE_IMP                      PJ_IOQUEUE_IMP_SELECT
#define PJ_FILE_IO                          PJ_FILE_IO_ANSI
#define PJ_QOS_IMPLEMENTATION               PJ_QOS_BSD

/* Zephyr Kconfig remains the source of truth for network protocol support. */
#if defined(CONFIG_NET_TCP)
#  define PJ_HAS_TCP                        1
#else
#  define PJ_HAS_TCP                        0
#endif

#if defined(CONFIG_NET_IPV6)
#  define PJ_HAS_IPV6                       1
#else
#  define PJ_HAS_IPV6                       0
#endif

#if defined(CONFIG_PJSIP)

/* Embedded PJSIP profile: TLS and AKA remain disabled. */
#define PJSIP_HAS_TLS_TRANSPORT             0
#define PJSIP_HAS_DIGEST_AKA_AUTH           0

#if defined(CONFIG_PJLIB_UTIL_DNS_RESOLVER)
#  define PJSIP_HAS_RESOLVER                1
#else
#  define PJSIP_HAS_RESOLVER                0
#endif

/* Kconfig owns the initial embedded resource limits. */
#define PJSIP_MAX_TSX_COUNT                 CONFIG_PJSIP_MAX_TSX_COUNT
#define PJSIP_MAX_DIALOG_COUNT              CONFIG_PJSIP_MAX_DIALOG_COUNT
#define PJSIP_MAX_TRANSPORTS                CONFIG_PJSIP_MAX_TRANSPORTS
#define PJSIP_TPMGR_HTABLE_SIZE             CONFIG_PJSIP_TPMGR_HTABLE_SIZE
#define PJSIP_MAX_MODULE                    CONFIG_PJSIP_MAX_MODULE
#define PJSIP_MAX_PKT_LEN                   CONFIG_PJSIP_MAX_PKT_LEN
#define PJSIP_MAX_TIMER_COUNT               CONFIG_PJSIP_MAX_TIMER_COUNT
#define PJSIP_MAX_NET_EVENTS                CONFIG_PJSIP_MAX_NET_EVENTS
#define PJSIP_MAX_TIMED_OUT_ENTRIES         CONFIG_PJSIP_MAX_TIMED_OUT_ENTRIES

#if PJSIP_MAX_TRANSPORTS > PJ_IOQUEUE_MAX_HANDLES
#  error "PJSIP_MAX_TRANSPORTS exceeds the PJLIB Zephyr ioqueue limit"
#endif

/* Keep Phase 4 pool sizing explicit and reproducible. */
#define PJSIP_POOL_LEN_ENDPT                16000
#define PJSIP_POOL_INC_ENDPT                4000
#define PJSIP_POOL_RDATA_LEN                4000
#define PJSIP_POOL_RDATA_INC                4000
#define PJSIP_POOL_LEN_TRANSPORT            512
#define PJSIP_POOL_INC_TRANSPORT            512
#define PJSIP_POOL_LEN_TDATA                4000
#define PJSIP_POOL_INC_TDATA                4000
#define PJSIP_POOL_LEN_UA                   512
#define PJSIP_POOL_INC_UA                   512
#define PJSIP_POOL_TSX_LAYER_LEN            512
#define PJSIP_POOL_TSX_LAYER_INC            512
#define PJSIP_POOL_TSX_LEN                  1536
#define PJSIP_POOL_TSX_INC                  256

#endif /* CONFIG_PJSIP */

#endif /* PJ_ZEPHYR || __ZEPHYR__ */

#endif /* __PJ_CONFIG_SITE_H__ */
