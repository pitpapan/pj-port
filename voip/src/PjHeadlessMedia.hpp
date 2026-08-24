#ifndef VOIP_PJ_HEADLESS_MEDIA_HPP
#define VOIP_PJ_HEADLESS_MEDIA_HPP

#include <voip/VoipFacade.hpp>

#include <pj/pool.h>
#include <pjmedia/endpoint.h>

namespace voip {

class PjHeadlessMedia final {
public:
    PjHeadlessMedia(pj_pool_factory *, pjmedia_endpt *) noexcept;
    Error Initialize() noexcept;
    Error Start(Codec) noexcept;
    Error SetPaused(bool) noexcept;
    Error Stop() noexcept;
    Error InjectTransportFailure() noexcept;
    MediaStats Stats() const noexcept;
    bool Running() const noexcept;
    void Destroy() noexcept;

private:
    struct State;
    State *state_;
};

} // namespace voip

#endif
