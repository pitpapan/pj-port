#include <mqtt_service.hpp>

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(mqtt_service, LOG_LEVEL_INF);

/* Last Will and Testament is sent at QoS 1 so the broker is guaranteed to
 * deliver the offline status even if it only gets one shot at it.
 */
static constexpr enum mqtt_qos kWillQos = MQTT_QOS_1_AT_LEAST_ONCE;

int MqttService::init(const Config &config)
{
	struct zsock_addrinfo hints = {};
	struct zsock_addrinfo *result = nullptr;
	char portStr[6];
	int rc;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	snprintk(portStr, sizeof(portStr), "%u", config.brokerPort);

	rc = zsock_getaddrinfo(config.brokerHost, portStr, &hints, &result);
	if (rc != 0) {
		LOG_ERR("Failed to resolve broker hostname [%s]", zsock_gai_strerror(rc));
		return -EIO;
	}
	if (result == nullptr) {
		LOG_ERR("Broker address not found");
		return -ENOENT;
	}

	memset(&broker_, 0, sizeof(broker_));
	memcpy(&broker_, result->ai_addr, result->ai_addrlen);
	zsock_freeaddrinfo(result);

	mqtt_client_init(&client_);

	const char *hostname = net_hostname_get();

	client_.broker = &broker_;
	client_.evt_cb = eventHandler;
	client_.user_data = this;
	client_.client_id.utf8 = reinterpret_cast<const uint8_t *>(hostname);
	client_.client_id.size = strlen(hostname);
	client_.protocol_version = MQTT_VERSION_5_0;
	client_.clean_session = config.cleanSession ? 1U : 0U;
	client_.keepalive = config.keepAlive;
	client_.transport.type = MQTT_TRANSPORT_NON_SECURE;

	client_.rx_buf = rxBuffer_;
	client_.rx_buf_size = sizeof(rxBuffer_);
	client_.tx_buf = txBuffer_;
	client_.tx_buf_size = sizeof(txBuffer_);

	if (config.username != nullptr) {
		username_.utf8 = reinterpret_cast<const uint8_t *>(config.username);
		username_.size = strlen(config.username);
		client_.user_name = &username_;

		if (config.password != nullptr) {
			password_.utf8 = reinterpret_cast<const uint8_t *>(config.password);
			password_.size = strlen(config.password);
			client_.password = &password_;
		}
	}

	snprintk(willTopicBuf_, sizeof(willTopicBuf_), "OnIT/user/%s/public/status", hostname);
	willTopic_.topic.utf8 = reinterpret_cast<const uint8_t *>(willTopicBuf_);
	willTopic_.topic.size = strlen(willTopicBuf_);
	willTopic_.qos = kWillQos;
	willMessage_.utf8 = reinterpret_cast<const uint8_t *>("0");
	willMessage_.size = 1;

	client_.will_topic = &willTopic_;
	client_.will_message = &willMessage_;
	client_.will_retain = 1U;

	return 0;
}

int MqttService::connect()
{
	int rc = mqtt_connect(&client_);

	if (rc != 0) {
		LOG_ERR("MQTT connect request failed [%d]", rc);
		return rc;
	}

	prepareFds();

	if (waitSocket(kSocketPollTimeoutMs) > 0) {
		mqtt_input(&client_);
	}

	if (!connected_) {
		mqtt_abort(&client_);
		clearFds();
		return -ENOTCONN;
	}

	return 0;
}

void MqttService::disconnect()
{
	mqtt_disconnect(&client_, nullptr);
	clearFds();
	connected_ = false;
}

void MqttService::process()
{
	if (!connected_) {
		return;
	}

	if (waitSocket(kSocketPollTimeoutMs) > 0) {
		int rc = mqtt_input(&client_);

		if (rc != 0) {
			LOG_ERR("mqtt_input failed [%d]", rc);
		}
	}

	/* Sends PINGREQ once the configured Keep Alive interval elapses. */
	int rc = mqtt_live(&client_);

	if (rc != 0 && rc != -EAGAIN) {
		LOG_ERR("mqtt_live failed [%d]", rc);
	}
}

void MqttService::setMessageHandler(SubscriptionHandler handler, void *context)
{
	messageHandler_ = handler;
	messageHandlerContext_ = context;
}

int MqttService::subscribe(const char *topic, enum mqtt_qos qos)
{
	struct mqtt_topic mqttTopic = {};

	mqttTopic.topic.utf8 = reinterpret_cast<const uint8_t *>(topic);
	mqttTopic.topic.size = strlen(topic);
	mqttTopic.qos = qos;

	struct mqtt_subscription_list subList = {};

	subList.list = &mqttTopic;
	subList.list_count = 1;
	subList.message_id = nextMessageId();

	int rc = mqtt_subscribe(&client_, &subList);

	if (rc != 0) {
		LOG_ERR("MQTT subscribe failed [%d]", rc);
	}

	return rc;
}

int MqttService::publish(const char *topic, const uint8_t *payload, size_t len, enum mqtt_qos qos)
{
	struct mqtt_publish_param param = {};

	param.message.topic.topic.utf8 = reinterpret_cast<const uint8_t *>(topic);
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = const_cast<uint8_t *>(payload);
	param.message.payload.len = len;
	param.message_id = (qos == MQTT_QOS_0_AT_MOST_ONCE) ? 0 : nextMessageId();

	return mqtt_publish(&client_, &param);
}

void MqttService::eventHandler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	auto *self = static_cast<MqttService *>(client->user_data);
	// there might be more than thousand topics publishing/subscripbing, so maybe this central event handler is not necessary

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed [%d]", evt->result);
			break;
		}
		self->connected_ = true;
		LOG_INF("MQTT client connected");
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected [%d]", evt->result);
		self->connected_ = false;
		self->clearFds();
		break;

	case MQTT_EVT_PUBLISH:
		self->handlePublish(evt);
		if (evt->param.publish.message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {};

			ack.message_id = evt->param.publish.message_id;
			mqtt_publish_qos1_ack(client, &ack);
		}
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error [%d]", evt->result);
		}
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT SUBACK error [%d]", evt->result);
		}
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("PINGRESP received");
		break;

	default:
		break;
	}
}

void MqttService::handlePublish(const struct mqtt_evt *evt)
{
	const struct mqtt_publish_param &publish = evt->param.publish;
	size_t len = publish.message.payload.len;

	if (messageHandler_ == nullptr) {
		mqtt_read_publish_payload(&client_, nullptr, 0);
		return;
	}

	if (len >= kPayloadBufSize) {
		LOG_WRN("Publish payload too large [%zu], dropping", len);
		mqtt_read_publish_payload(&client_, nullptr, 0);
		return;
	}

	uint8_t payload[kPayloadBufSize];
	int rc = mqtt_readall_publish_payload(&client_, payload, len);

	if (rc != 0) {
		LOG_ERR("Failed to read publish payload [%d]", rc);
		return;
	}
	payload[len] = '\0';

	char topic[kPayloadBufSize];
	size_t topicLen = publish.message.topic.topic.size;

	if (topicLen >= sizeof(topic)) {
		topicLen = sizeof(topic) - 1;
	}
	memcpy(topic, publish.message.topic.topic.utf8, topicLen);
	topic[topicLen] = '\0';

	messageHandler_(topic, payload, len, messageHandlerContext_);
}

void MqttService::prepareFds()
{
	fds_[0].fd = client_.transport.tcp.sock;
	fds_[0].events = ZSOCK_POLLIN;
	nfds_ = 1;
}

void MqttService::clearFds()
{
	nfds_ = 0;
}

int MqttService::waitSocket(int timeout_ms)
{
	if (nfds_ <= 0) {
		return 0;
	}

	int rc = zsock_poll(fds_, nfds_, timeout_ms);

	if (rc < 0) {
		LOG_ERR("poll error [%d]", errno);
	}

	return rc;
}

uint16_t MqttService::nextMessageId()
{
	if (nextMessageId_ == 0) {
		nextMessageId_ = 1;
	}

	return nextMessageId_++;
}
