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

#if defined(CONFIG_PJMEDIA)

/* Zephyr's initial PJMEDIA profile is audio-only and dependency-minimal. */
#define PJMEDIA_HAS_VIDEO                    0
#define PJMEDIA_HAS_SRTP                     0
#define PJMEDIA_SRTP_HAS_SDES                0
#define PJMEDIA_SRTP_HAS_DTLS                0
#define PJMEDIA_HAS_RTCP_XR                  0
#define PJMEDIA_STREAM_ENABLE_XR             0
#define PJMEDIA_HAS_LEGACY_SOUND_API         0
#define PJMEDIA_RESAMPLE_IMP                 PJMEDIA_RESAMPLE_NONE
#define PJMEDIA_HAS_SPEEX_AEC                0
#define PJMEDIA_HAS_WEBRTC_AEC               0
#define PJMEDIA_HAS_WEBRTC_AEC3              0
#define PJMEDIA_HAS_LIBYUV                   0
#define PJMEDIA_HAS_FFMPEG                   0

/* Keep the first SDP and negotiation bounds explicit and reproducible. */
#define PJMEDIA_MAX_SDP_MEDIA                4
#define PJMEDIA_MAX_SDP_FMT                  16
#define PJMEDIA_SDP_NEG_MAX_CUSTOM_FMT_NEG_CB 4

/* G.711 is the only codec in the initial embedded profile. */
#if defined(CONFIG_PJMEDIA_G711)
#  define PJMEDIA_HAS_G711_CODEC             1
#else
#  define PJMEDIA_HAS_G711_CODEC             0
#endif
#define PJMEDIA_HAS_ALAW_ULAW_TABLE          0
#define PJMEDIA_HAS_L16_CODEC                0
#define PJMEDIA_HAS_GSM_CODEC                0
#define PJMEDIA_HAS_SPEEX_CODEC              0
#define PJMEDIA_HAS_ILBC_CODEC               0
#define PJMEDIA_HAS_G722_CODEC               0
#define PJMEDIA_HAS_G7221_CODEC              0
#define PJMEDIA_HAS_INTEL_IPP                0
#define PJMEDIA_HAS_PASSTHROUGH_CODECS       0
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC     0
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC     0
#define PJMEDIA_HAS_SILK_CODEC               0
#define PJMEDIA_HAS_OPUS_CODEC               0
#define PJMEDIA_HAS_BCG729                   0
#define PJMEDIA_HAS_LYRA_CODEC               0
#define PJMEDIA_HAS_FFMPEG_CODEC             0
#define PJMEDIA_HAS_VPX_CODEC_VP8            0
#define PJMEDIA_HAS_VPX_CODEC_VP9            0

/* Host audio backends are never inherited by the Zephyr configuration. */
#define PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO      0
#define PJMEDIA_AUDIO_DEV_HAS_OPENSL         0
#define PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI    0
#define PJMEDIA_AUDIO_DEV_HAS_OBOE           0
#define PJMEDIA_AUDIO_DEV_HAS_BB10           0
#define PJMEDIA_AUDIO_DEV_HAS_ALSA           0
#if defined(CONFIG_PJMEDIA_AUDIODEV_NULL)
#  define PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO   1
#else
#  define PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO   0
#endif
#define PJMEDIA_AUDIO_DEV_HAS_COREAUDIO      0
#define PJMEDIA_AUDIO_DEV_HAS_WMME           0
#define PJMEDIA_AUDIO_DEV_HAS_WASAPI         0
#define PJMEDIA_AUDIO_DEV_HAS_BDIMAD         0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_APS       0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_VAS       0
#define PJMEDIA_AUDIO_DEV_HAS_SYMB_MDA       0
#define PJMEDIA_AUDIO_DEV_HAS_LEGACY_DEVICE  0

#endif /* CONFIG_PJMEDIA */

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
