#include <zephyr/sys/printk.h>

#include <pjmedia/config.h>
#include <pjmedia/sdp.h>
#include <pjmedia-codec/config.h>
#include <pjmedia-audiodev/config.h>

_Static_assert(PJMEDIA_HAS_VIDEO == 0, "video must remain disabled");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "SRTP must remain disabled");
_Static_assert(PJMEDIA_HAS_RTCP_XR == 0, "RTCP XR must remain disabled");
_Static_assert(PJMEDIA_STREAM_ENABLE_XR == 0,
	       "stream RTCP XR must remain disabled");
_Static_assert(PJMEDIA_HAS_LEGACY_SOUND_API == 0,
	       "legacy sound API must remain disabled");
_Static_assert(PJMEDIA_RESAMPLE_IMP == PJMEDIA_RESAMPLE_NONE,
	       "resampling must remain disabled");
_Static_assert(PJMEDIA_HAS_SPEEX_AEC == 0, "Speex AEC must remain disabled");
_Static_assert(PJMEDIA_HAS_WEBRTC_AEC == 0,
	       "WebRTC AEC must remain disabled");
_Static_assert(PJMEDIA_HAS_WEBRTC_AEC3 == 0,
	       "WebRTC AEC3 must remain disabled");
_Static_assert(PJMEDIA_HAS_G711_CODEC == 1,
	       "G.711 must be the selected initial codec");
_Static_assert(PJMEDIA_HAS_L16_CODEC == 0, "L16 must remain disabled");
_Static_assert(PJMEDIA_HAS_GSM_CODEC == 0, "GSM must remain disabled");
_Static_assert(PJMEDIA_HAS_SPEEX_CODEC == 0, "Speex must remain disabled");
_Static_assert(PJMEDIA_HAS_ILBC_CODEC == 0, "iLBC must remain disabled");
_Static_assert(PJMEDIA_HAS_G722_CODEC == 0, "G.722 must remain disabled");
_Static_assert(PJMEDIA_HAS_PASSTHROUGH_CODECS == 0,
	       "passthrough codecs must remain disabled");
_Static_assert(PJMEDIA_MAX_SDP_MEDIA == 4, "unexpected SDP media limit");
_Static_assert(PJMEDIA_MAX_SDP_FMT == 16, "unexpected SDP format limit");
_Static_assert(PJMEDIA_SDP_NEG_MAX_CUSTOM_FMT_NEG_CB == 4,
	       "unexpected SDP negotiation callback limit");
_Static_assert(PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO == 0,
	       "PortAudio must remain disabled");
_Static_assert(PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI == 0,
	       "Android audio must remain disabled");
_Static_assert(PJMEDIA_AUDIO_DEV_HAS_ALSA == 0,
	       "ALSA must remain disabled");
_Static_assert(PJMEDIA_AUDIO_DEV_HAS_WMME == 0,
	       "WMME must remain disabled");
_Static_assert(PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO == 0,
	       "null audio is deferred");

int phase1_boundary_run(void)
{
	printk("PJMEDIA Phase 1 configuration/header boundary: PASSED\n");
	printk("No PJMEDIA production object is linked in Phase 1\n");
	return 0;
}
