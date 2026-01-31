#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <jsoncpp/json/json.h>

#if ORTHO_ENABLE_GRPC
#include <grpcpp/grpcpp.h>
#endif

#include "ironsoft/mqtt_config.h"
#include "ironsoft/topics.h"
#include "mqtt_client.h"
#include "state.h"

using ironsoft::MqttConfig;
using ironsoft::make_topics;
using ironsoft::MqttBackendClient;
using ironsoft::BackendState;
using ironsoft::CmdHandlerResult;

static void usage() {
  std::cout << "Usage: ironsoft_backend --config <path_to_mqtt_json>\n";
}

static bool load_json(const std::string& path, Json::Value& out) {
  std::ifstream f(path);
  if (!f) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  return Json::parseFromStream(rb, f, &out, &errs);
}

static MqttConfig parse_mqtt_config(const Json::Value& root) {
  MqttConfig cfg;
  cfg.host = root["broker"]["host"].asString();
  cfg.port = root["broker"]["port"].asInt();

  if (root.isMember("auth")) {
    cfg.username = root["auth"]["username"].asString();
    cfg.password = root["auth"]["password"].asString();
  }
  if (root.isMember("tls")) {
    cfg.tls_enabled = root["tls"]["enabled"].asBool();
    cfg.ca_file = root["tls"]["ca_file"].asString();
  }
  if (root.isMember("client")) {
    cfg.drone_id = root["client"]["drone_id"].asString();
    cfg.keepalive_s = root["client"]["keepalive_s"].asInt();
  }
  return cfg;
}

static CmdHandlerResult handle_cmd_ping_only(const std::string& cmd_json) {
  // MVP: support only {"type":"ping"}
  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  std::istringstream iss(cmd_json);
  if (!Json::parseFromStream(rb, iss, &root, &errs)) {
    return {false, "", "BAD_JSON"};
  }
  const std::string type = root.get("type", "").asString();
  if (type == "ping") return {true, "pong", ""};
  return {false, "", "UNKNOWN_CMD"};
}

int main(int argc, char** argv) {
  std::string cfg_path;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      cfg_path = argv[++i];
    }
  }

  if (cfg_path.empty()) {
    usage();
    return 2;
  }

  Json::Value cfg_json;
  if (!load_json(cfg_path, cfg_json)) {
    std::cerr << "Failed to read config: " << cfg_path << std::endl;
    return 3;
  }

  MqttConfig cfg = parse_mqtt_config(cfg_json);
  auto topics = make_topics(cfg.drone_id);

  BackendState st;

  std::cout << "IronSoft backend starting\n"
            << "Broker: " << cfg.host << ":" << cfg.port << "\n"
            << "Drone ID: " << cfg.drone_id << "\n"
            << "Topics prefix: " << topics.prefix << "\n";

#if !ORTHO_ENABLE_GRPC
  std::cout << "gRPC disabled\n";
#endif

  MqttBackendClient mqtt(cfg, topics, st);
  mqtt.start(handle_cmd_ping_only);

  std::cout << "Press ENTER to exit...\n";
  std::string dummy;
  std::getline(std::cin, dummy);

  mqtt.stop();
  return 0;
}
