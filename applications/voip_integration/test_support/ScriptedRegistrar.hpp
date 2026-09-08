#ifndef VOIP_INTEGRATION_SCRIPTED_REGISTRAR_HPP
#define VOIP_INTEGRATION_SCRIPTED_REGISTRAR_HPP

#include <cstdint>

namespace voip_integration {

struct RegistrarStats {
    unsigned requests;
    unsigned tcp_requests;
    unsigned non_tcp_requests;
    unsigned challenges;
    unsigned authorized;
    unsigned forbidden;
    unsigned registrations;
    unsigned unregistrations;
    unsigned callback_failures;
    unsigned registrations_by_agent[5];
};

class ScriptedRegistrar final {
public:
    static constexpr unsigned listener_port = 5097;

    static ScriptedRegistrar &Instance() noexcept;
    void Reset() noexcept;
    void ArmOne408(unsigned agent_index) noexcept;
    void UseShortSuccessfulExpiration(unsigned agent_index) noexcept;
    RegistrarStats Stats() const noexcept;

private:
    ScriptedRegistrar() noexcept = default;
};

} // namespace voip_integration

#endif
