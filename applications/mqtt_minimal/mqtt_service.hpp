#pragma once

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
extern bool mqtt_connected;

class MqttService
{
public:
    static APP_BMEM uint8_t rx_buffer[MQTT_BUFFER_SIZE];
    static APP_BMEM uint8_t tx_buffer[MQTT_BUFFER_SIZE];
    struct Config
    {
        const char* brokerHost{};
        __UINT16_C brokerPort{0};
        const char* clientId{}; // by default, dhcp hostname
        const char* username{}; // security package
        const char* password{}; // maybe i should not struct it into config?
        __UINT16_C keppAlive{}; // defomed nu MqttKeepAlive 
        bool cleanSession{false}; // MqttKeepAlive
        //TODO TLS
    }

    MqttService();
    ~MqttService() = default;
    
    int init(Config &config);
    void start();
    void stop();
    bool isConnected() {return mqtt_connected;};

    // client
    int registerSubscription(const char* topic, Qos qos, Handler, handler, void* context);
    void connect();
    
    //publisher
    void publish();
private:
    bool mqtt_connected{false};

    void process();
    int poll_mqtt_socket(struct mqtt_client *client);
    void prepare_fds();
    void clear_fds();
}