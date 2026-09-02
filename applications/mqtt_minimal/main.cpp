#include <mqtt_service.hpp>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mqtt_minimal, LOG_LEVEL_INF);

/* Stand-ins until NetworkConfig and the security package exist: broker
 * address/keep-alive/clean-session and credentials are hardcoded here.
 */
#define MQTT_BROKER_HOST "10.0.2.2"
#define MQTT_BROKER_PORT 1884
#define MQTT_KEEP_ALIVE_S 60
#define MQTT_CLEAN_SESSION true
#define MQTT_USERNAME     nullptr
#define MQTT_PASSWORD     nullptr

#define MQTT_POLL_INTERVAL K_MSEC(100)

static MqttService mqtt_service;
static struct k_work_delayable mqtt_work;

/* Runs on the system workqueue, so no dedicated thread/stack is needed
 * just to pump the MQTT client.
 */
static void mqtt_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (mqtt_service.isConnected()) {
		mqtt_service.process();
	} else {
		mqtt_service.connect();
	}

	k_work_reschedule(&mqtt_work, MQTT_POLL_INTERVAL);
}

int main(void)
{
	MqttService::Config config{};

	config.brokerHost = MQTT_BROKER_HOST;
	config.brokerPort = MQTT_BROKER_PORT;
	config.username = MQTT_USERNAME;
	config.password = MQTT_PASSWORD;
	config.keepAlive = MQTT_KEEP_ALIVE_S;
	config.cleanSession = MQTT_CLEAN_SESSION;

	int rc = mqtt_service.init(config);

	if (rc != 0) {
		LOG_ERR("MQTT service init failed [%d]", rc);
		return rc;
	}

	k_work_init_delayable(&mqtt_work, mqtt_work_handler);
	k_work_schedule(&mqtt_work, K_NO_WAIT);

	return 0;
}
