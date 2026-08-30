#ifndef VOIP_PJSUA_SIGNALING_TRANSPORT_POLICY_HPP
#define VOIP_PJSUA_SIGNALING_TRANSPORT_POLICY_HPP

#include <cstdint>
namespace voip { enum class SignalingTransportPolicy : std::uint8_t { tcp_plain, tls }; }
#endif
