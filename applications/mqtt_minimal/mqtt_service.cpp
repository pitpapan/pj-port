 #include <mqtt_service.hpp>

 static inline void connect()
 {
    int rc = 0;

    while (!mqtt_connected) {
        rc = mqtt_connect(client)
        if (rc != 0){
            LOG_ERR("MQTT connection failed [%d]", rc);
            k_msleep(10);
            continue
        }

        /* Poll MQTT socket for response */
		rc = poll_mqtt_socket(client, MSECS_NET_POLL_TIMEOUT);
		if (rc > 0) {
			mqtt_input(client);
		}

		if (!mqtt_connected) {
			mqtt_abort(client);
		}
    }
 }

 int init(Config &config)
 {
    int rc;
    	struct addrinfo *result;
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

    /* Resolve IP address of MQTT broker */
	rc = getaddrinfo(config.brokerHost,
				config.brokerPort, &hints, &result);
    if (rc != 0) {
        LOG_ERR("Failed to resolve broker hostname [%s]", gai_strerror(rc));
		return -EIO;
	}
	if (result == NULL) {
		LOG_ERR("Broker address not found");
		return -ENOENT;
    }


    // MQTT client configuration
    init_mqtt_client_id();
	mqtt_client_init(client);
	client->broker = &broker;
	client->evt_cb = mqtt_event_handler;
	client->client_id.utf8 = client_id;
	client->client_id.size = strlen(client->client_id.utf8);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1; //5?
	client->clean_session = config->cleanSession; //persistent session
	// MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

    // TODO TLS
    /**/


 }

 void start()
 {
    int rc;
 }

 void stop()
 {

 }

 int registerSubscription(const char* topic, Qos qos, Handler, handler, void* context)
 {
    int rc;
    rc = 2bscribe()
    struct mqtt_subscription_list sub_list = 
    {
        .list = topic,
        .list_count = 1,
        .message_id = 1 //i have to check to to determinate the id
    };
    rc = mqtt_subscribe(client, &sub_list);
	if (rc != 0) {
		LOG_ERR("MQTT Subscribe failed [%d]", rc);
	}

	return rc;
 }

 void process()
{
	int rc

}

static void prepare_fds(struct mqtt_client *client)
{
	if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds[0].fd = client->transport.tcp.sock;
	}
#if defined(CONFIG_MQTT_LIB_TLS)
	else if (client->transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = client->transport.tls.sock;
	}
#endif

	fds[0].events = POLLIN;
	nfds = 1;
}

static void clear_fds(void)
{
	nfds = 0;
}