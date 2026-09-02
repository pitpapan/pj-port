#pragma once

#include <cstddef>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

/*
 * Minimal MQTT 5.0 client wrapper around Zephyr's MQTT library.
 *
 * Requirements covered:
 *  - MQTT 5.0 protocol.
 *  - Connects to the broker given in Config (NetworkConfig.MqttBroker).
 *  - Client ID is always the DHCP-provided hostname (net_hostname_get()).
 *  - Clean session behavior follows Config.cleanSession (MqttCleanSession).
 *  - Username/password come from the security package via Config.
 *  - LWT is set to "OnIT/user/{hostname}/public/status" = "0".
 *  - Keep alive interval follows Config.keepAlive (MqttKeepAlive); PINGREQ/
 *    PINGRESP handling is driven by calling process() periodically, which
 *    delegates to mqtt_live().
 *  - Multiple independent modules (speaker manager, microphone manager,
 *    etc.) can each subscribe() their own topic with their own handler.
 */
class MqttService
{
public:
	using SubscriptionHandler = void (*)(const char *topic, const uint8_t *payload,
					      size_t len, void *context);

	struct Config {
		const char *brokerHost{};  /* NetworkConfig.MqttBroker */
		uint16_t brokerPort{0};    /* NetworkConfig.MqttBroker */
		const char *username{};    /* provided by the security package */
		const char *password{};    /* provided by the security package */
		uint16_t keepAlive{0};     /* NetworkConfig.MqttKeepAlive, in seconds */
		bool cleanSession{false};  /* NetworkConfig.MqttCleanSession */
	};

	MqttService() = default;
	~MqttService() = default;

	/* Resolves the broker address and configures the client. Must be
	 * called once before connect().
	 */
	int init(const Config &config);

	/* Attempts a single connection handshake. Returns 0 once the broker
	 * has acknowledged the connection (isConnected() becomes true).
	 * On failure, the caller is expected to retry with its own backoff.
	 */
	int connect();
	void disconnect();

	/* Shall be called periodically by the application to pump socket I/O
	 * and to let the library send PINGREQ per the Keep Alive interval.
	 */
	void process();

	bool isConnected() const { return connected_; }

	/* Subscribes to a topic and registers the handler invoked for every
	 * PUBLISH received on it. Each caller (speaker manager, microphone
	 * manager, etc.) owns its own topic/handler pair; up to
	 * kMaxSubscriptions can be registered. `topic` must outlive the
	 * subscription (a string literal or otherwise static storage).
	 */
	int subscribe(const char *topic, enum mqtt_qos qos, SubscriptionHandler handler, void *context);
	int publish(const char *topic, const uint8_t *payload, size_t len, enum mqtt_qos qos);

private:
	static constexpr size_t kBufferSize = 256;
	static constexpr size_t kPayloadBufSize = 128;
	static constexpr size_t kMaxSubscriptions = 4;
	static constexpr int kSocketPollTimeoutMs = 100;

	struct Subscription {
		const char *topic{};
		SubscriptionHandler handler{};
		void *context{};
	};

	static void eventHandler(struct mqtt_client *client, const struct mqtt_evt *evt);
	void handlePublish(const struct mqtt_evt *evt);
	Subscription *dispatchPublish(const char *topic);
	void discardPublishPayload(size_t len);
	void prepareFds();
	void clearFds();
	int waitSocket(int timeout_ms);
	uint16_t nextMessageId();

	struct mqtt_client client_{};
	struct sockaddr_storage broker_{};
	struct zsock_pollfd fds_[1]{};
	int nfds_{0};
	bool connected_{false};

	uint8_t rxBuffer_[kBufferSize]{};
	uint8_t txBuffer_[kBufferSize]{};

	struct mqtt_utf8 username_{};
	struct mqtt_utf8 password_{};

	struct mqtt_topic willTopic_{};
	struct mqtt_utf8 willMessage_{};
	char willTopicBuf_[NET_HOSTNAME_MAX_LEN + 32]{};

	Subscription subscriptions_[kMaxSubscriptions]{};
	size_t subscriptionCount_{0};
	uint16_t nextMessageId_{1};
};
