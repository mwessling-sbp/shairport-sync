#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "common.h"
#include "player.h"
#include "rtsp.h"

#include "rtp.h"

#include "dacp.h"
#include "mqtt.h"
#include <mosquitto.h>

#include "cJSON.h"

// this holds the mosquitto client
struct mosquitto *global_mosq = NULL;
char *topic = NULL;
int connected = 0;

// mosquitto logging
void _cb_log(__attribute__((unused)) struct mosquitto *mosq, __attribute__((unused)) void *userdata,
             int level, const char *str)
{
  switch (level)
  {
  case MOSQ_LOG_DEBUG:
    debug(1, str);
    break;
  case MOSQ_LOG_INFO:
    debug(2, str);
    break;
  case MOSQ_LOG_NOTICE:
    debug(3, str);
    break;
  case MOSQ_LOG_WARNING:
    inform(str);
    break;
  case MOSQ_LOG_ERR:
  {
    die("MQTT: Error: %s\n", str);
  }
  }
}

// mosquitto message handler
void on_message(__attribute__((unused)) struct mosquitto *mosq,
                __attribute__((unused)) void *userdata, const struct mosquitto_message *msg)
{

  // null-terminate the payload
  char payload[msg->payloadlen + 1];
  memcpy(payload, msg->payload, msg->payloadlen);
  payload[msg->payloadlen] = 0;

  debug(1, "[MQTT]: received Message on topic %s: %s\n", msg->topic, payload);

  // All recognized commands
  char *commands[] = {"command", "beginff", "beginrew", "mutetoggle", "nextitem",
                      "previtem", "pause", "playpause", "play", "stop",
                      "playresume", "shuffle_songs", "volumedown", "volumeup", NULL};

  int it = 0;

  // send command if it's a valid one
  while (commands[it] != NULL)
  {
    if ((size_t)msg->payloadlen >= strlen(commands[it]) &&
        strncmp(msg->payload, commands[it], strlen(commands[it])) == 0)
    {
      debug(1, "[MQTT]: DACP Command: %s\n", commands[it]);
      send_simple_dacp_command(commands[it]);
      break;
    }
    it++;
  }
}

void on_disconnect(__attribute__((unused)) struct mosquitto *mosq,
                   __attribute__((unused)) void *userdata, __attribute__((unused)) int rc)
{
  connected = 0;
  debug(1, "[MQTT]: disconnected");
}

void on_connect(struct mosquitto *mosq, __attribute__((unused)) void *userdata,
                __attribute__((unused)) int rc)
{
  connected = 1;
  debug(1, "[MQTT]: connected");

  // subscribe if requested
  if (config.mqtt_enable_remote)
  {
    char remotetopic[strlen(config.mqtt_topic) + 8];
    snprintf(remotetopic, strlen(config.mqtt_topic) + 8, "%s/remote", config.mqtt_topic);
    mosquitto_subscribe(mosq, NULL, remotetopic, 0);
  }
}

// helper function to publish under a topic and automatically append the main topic
void mqtt_publish(char *topic, char *data, uint32_t length, int is_event)
{
  char fulltopic[strlen(config.mqtt_topic) + strlen(topic) + 3];
  char *jmsg_str;
  debug(2, "[MQTT]: topic %s, data %s, %d", topic, data, length);

  // Publish in JSON format
  if (config.mqtt_publish_json)
  {
    char subtopic[] = "airplay";

    snprintf(fulltopic, strlen(config.mqtt_topic) + strlen(subtopic) + 2, "%s/%s", config.mqtt_topic, subtopic);

    cJSON *jmsg = cJSON_CreateObject();
    if (jmsg == NULL)
    {
      debug(1, "[MQTT]: json object creation failed");
      goto end;
    }
    cJSON *jvalue = NULL;

    if (length > 0)
    {
      char jvalue_str[strlen(data) + 1];

      strncpy(jvalue_str, data, length);
      jvalue_str[length] = '\0';
      debug(1, "[MQTT]: json jvalue_str %s, %d", jvalue_str, length);
      jvalue = cJSON_CreateString(jvalue_str);
      if (jvalue == NULL)
      {
        debug(1, "[MQTT]: json jvalue creation from %s failed", jvalue_str);
      }
      else
      {
        cJSON_AddItemToObject(jmsg, topic, jvalue);
      }
    }
    else if (is_event)
    {
      debug(1, "[MQTT]: json jevent_str %s", topic);
      jvalue = cJSON_CreateString(topic);
      if (jvalue == NULL)
      {
        debug(1, "[MQTT]: json jvalue creation from %s failed", topic);
      }
      else
      {
        cJSON_AddItemToObject(jmsg, "event", jvalue);
      }
    }
    else
    { // length == 0

      jvalue = cJSON_CreateNull();
      if (jvalue == NULL)
      {
        debug(1, "[MQTT]: json jvalue creation NULL failed");
      }
      else
      {
        cJSON_AddItemToObject(jmsg, topic, jvalue);
      }
    }

    jmsg_str = cJSON_Print(jmsg);
    if (jmsg_str == NULL)
    {
      debug(1, "[MQTT]: json msg string conversion failed");
      goto end;
    }

    int rc;
    debug(1, "[MQTT]: publishing under %s", fulltopic);
    debug(2, "[MQTT]: message %s", jmsg_str);

    if ((rc = mosquitto_publish(global_mosq, NULL, fulltopic, strlen(jmsg_str), jmsg_str, 0, 0)) !=
        MOSQ_ERR_SUCCESS)
    {
      switch (rc)
      {
      case MOSQ_ERR_NO_CONN:
        debug(1, "[MQTT]: Publish failed: not connected to broker");
        break;
      default:
        debug(1, "[MQTT]: Publish failed: unknown error");
        break;
      }
    }
    free(jmsg_str);
  end:
    cJSON_Delete(jmsg);
  }
  else
  {

    snprintf(fulltopic, strlen(config.mqtt_topic) + strlen(topic) + 2, "%s/%s", config.mqtt_topic, topic);
    int rc;
    debug(1, "[MQTT]: publishing under %s", fulltopic);
    debug(2, "[MQTT]: message %s", data);
    if ((rc = mosquitto_publish(global_mosq, NULL, fulltopic, length, data, 0, 0)) !=
        MOSQ_ERR_SUCCESS)
    {
      switch (rc)
      {
      case MOSQ_ERR_NO_CONN:
        debug(1, "[MQTT]: Publish failed: not connected to broker");
        break;
      default:
        debug(1, "[MQTT]: Publish failed: unknown error");
        break;
      }
    }
  }
}

// handler for incoming metadata
void mqtt_process_metadata(uint32_t type, uint32_t code, char *data, uint32_t length)
{
  if (global_mosq == NULL || connected != 1)
  {
    debug(3, "[MQTT]: Client not connected, skipping metadata handling");
    return;
  }
  if (config.mqtt_publish_raw)
  {
    uint32_t val;
    char topic[] = "____/____";

    val = htonl(type);
    memcpy(topic, &val, 4);
    val = htonl(code);
    memcpy(topic + 5, &val, 4);
    mqtt_publish(topic, data, length,MQTT_VALUE);
  }
  if (config.mqtt_publish_parsed)
  {
    if (type == 'core')
    {
      switch (code)
      {
      case 'asar':
        mqtt_publish("artist", data, length,MQTT_VALUE);
        break;
      case 'asal':
        mqtt_publish("album", data, length,MQTT_VALUE);
        break;
      case 'minm':
        mqtt_publish("title", data, length,MQTT_VALUE);
        break;
      case 'asgn':
        mqtt_publish("genre", data, length,MQTT_VALUE);
        break;
      case 'asfm':
        mqtt_publish("format", data, length,MQTT_VALUE);
        break;
      }
    }
    else if (type == 'ssnc')
    {
      switch (code)
      {
      case 'asal':
        mqtt_publish("songalbum", data, length,MQTT_VALUE);
        break;
      case 'pvol':
        mqtt_publish("volume", data, length,MQTT_VALUE);
        break;
      case 'clip':
        mqtt_publish("client_ip", data, length,MQTT_VALUE);
        break;
      case 'abeg':
        mqtt_publish("active_start", data, length,MQTT_EVENT);
        break;
      case 'aend':
        mqtt_publish("active_end", data, length,MQTT_EVENT);
        break;
      case 'pbeg':
        mqtt_publish("play_start", data, length,MQTT_EVENT);
        break;
      case 'pend':
        mqtt_publish("play_end", data, length,MQTT_EVENT);
        break;
      case 'pfls':
        mqtt_publish("play_flush", data, length,MQTT_EVENT);
        break;
      case 'prsm':
        mqtt_publish("play_resume", data, length,MQTT_EVENT);
        break;
      case 'PICT':
        if (config.mqtt_publish_cover)
        {
          mqtt_publish("cover", data, length,MQTT_VALUE);
        }
        break;
      }
    }
  }

  return;
}

int initialise_mqtt()
{
  debug(1, "Initialising MQTT");
  if (config.mqtt_hostname == NULL)
  {
    debug(1, "[MQTT]: Not initialized, as the hostname is not set");
    return 0;
  }
  int keepalive = 60;
  mosquitto_lib_init();
  if (!(global_mosq = mosquitto_new(config.service_name, true, NULL)))
  {
    die("[MQTT]: FATAL: Could not create mosquitto object! %d\n", global_mosq);
  }

  if (config.mqtt_cafile != NULL || config.mqtt_capath != NULL || config.mqtt_certfile != NULL ||
      config.mqtt_keyfile != NULL)
  {
    if (mosquitto_tls_set(global_mosq, config.mqtt_cafile, config.mqtt_capath, config.mqtt_certfile,
                          config.mqtt_keyfile, NULL) != MOSQ_ERR_SUCCESS)
    {
      die("[MQTT]: TLS Setup failed");
    }
  }

  if (config.mqtt_username != NULL || config.mqtt_password != NULL)
  {
    if (mosquitto_username_pw_set(global_mosq, config.mqtt_username, config.mqtt_password) !=
        MOSQ_ERR_SUCCESS)
    {
      die("[MQTT]: Username/Password set failed");
    }
  }
  mosquitto_log_callback_set(global_mosq, _cb_log);

  if (config.mqtt_enable_remote)
  {
    mosquitto_message_callback_set(global_mosq, on_message);
  }

  mosquitto_disconnect_callback_set(global_mosq, on_disconnect);
  mosquitto_connect_callback_set(global_mosq, on_connect);
  if (mosquitto_connect(global_mosq, config.mqtt_hostname, config.mqtt_port, keepalive))
  {
    inform("[MQTT]: Could not establish a mqtt connection");
  }
  if (mosquitto_loop_start(global_mosq) != MOSQ_ERR_SUCCESS)
  {
    inform("[MQTT]: Could start MQTT Main loop");
  }

  return 0;
}
