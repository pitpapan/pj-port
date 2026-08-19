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

#endif /* PJ_ZEPHYR || __ZEPHYR__ */

#endif /* __PJ_CONFIG_SITE_H__ */
