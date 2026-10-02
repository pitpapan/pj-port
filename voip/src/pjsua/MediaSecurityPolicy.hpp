#ifndef VOIP_MEDIA_SECURITY_POLICY_HPP
#define VOIP_MEDIA_SECURITY_POLICY_HPP
#include <voip/VoipTypes.hpp>
namespace voip {
struct MediaSecurityPolicy {
    static Error Validate(MediaSecurity security) noexcept {
        if (security==MediaSecurity::none) return Error::ok;
        if (security==MediaSecurity::srtp_sdes) return Error::unsupported_configuration;
        return Error::invalid_argument;
    }
};
}
#endif
