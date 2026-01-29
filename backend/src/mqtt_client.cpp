#include "mqtt_client.h"

#include <iostream>
#include <sstream>
#include <thread>

#include <jsoncpp/json/json.h>

#include "ironsoft/util_time.h"

namespace ironsoft {

static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

MqttBackendClient::MqttBackendClient(MqttConfig cfg, Topics topics, BackendState& st)
  : cfg_(std::move(cfg)), topics_(std::move(topics)), st_(st) {

  const std::string server_uri = "tcp://" + cfg_.host + ":" + std::to_string(cfg_.port);
  const std::string client_id = "ironsoft-backend-" + cfg_.drone_id;

  client_ = std::make_unique<mqtt::async_client>(server_uri, client_id);

  // LWT: publish offline if we disappear.
  Json::Value offline;
  offline["state"] = "offline";
  offline["ts"] = Json::Int64(now_unix_s());
  offline["reason"] = "lwt";

  mqtt::will_options will(topics_.presence, json_stringify(offline), /*qos*/1, /*retained*/true);

  mqtt::connect_options_builder builder;
  builder.keep_alive_interval(std::chrono::seconds(cfg_.keepalive_s));
  builder.clean_session(true);
  builder.will(will);

  if (!cfg_.username.empty()) {
    builder.user_name(cfg_.username);
    builder.password(cfg_.password);
  }

  connopts_ = builder.finalize();
}

MqttBackendClient::~MqttBackendClient() {
  stop();
}

void MqttBackendClient::start(CmdHandler on_cmd) {
  on_cmd_ = std::move(on_cmd);
  stop_requested_ = false;

  connect_thread_ = std::thread([this]() { run_connect_loop(); });
  heartbeat_thread_ = std::thread([this]() { publish_heartbeat_loop(); });
}

void MqttBackendClient::stop() {
  stop_requested_ = true;
  st_.running = false;

  if (connect_thread_.joinable()) connect_thread_.join();
  if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

  try {
    if (client_ && st_.connected.load()) {
      client_->disconnect()->wait();
    }
  } catch (...) {
    // ignore
  }
}

void MqttBackendClient::run_connect_loop() {
  while (!stop_requested_) {
    try {
      std::cout << "[mqtt] connecting to " << cfg_.host << ":" << cfg_.port << std::endl;
      client_->connect(connopts_)->wait();
      st_.connected = true;
      std::cout << "[mqtt] connected" << std::endl;

      publish_presence_online();
      publish_status_retained();
      subscribe_cmd();

      // message loop (blocking)
      while (!stop_requested_) {
        auto msg = client_->consume_message();
        if (!msg) {
          // consume_message may return null if disconnected
          break;
        }
        if (msg->get_topic() == topics_.cmd) {
          const std::string payload = msg->to_string();
          CmdHandlerResult res{true, "ok", ""};
          if (on_cmd_) res = on_cmd_(payload);

          // Parse id (best-effort)
          std::string cmd_id = "";
          try {
            Json::CharReaderBuilder rb;
            Json::Value root;
            std::string errs;
            std::istringstream iss(payload);
            if (Json::parseFromStream(rb, iss, &root, &errs)) {
              if (root.isMember("id")) cmd_id = root["id"].asString();
            }
          } catch (...) {}

          Json::Value ack;
          ack["id"] = cmd_id;
          ack["ok"] = res.ok;
          if (res.ok) ack["message"] = res.message;
          else ack["error"] = res.error;

          client_->publish(topics_.ack, json_stringify(ack), /*qos*/1, /*retained*/false);
        }
      }
    } catch (const std::exception& e) {
      st_.connected = false;
      std::cerr << "[mqtt] connect loop error: " << e.what() << std::endl;
    }

    st_.connected = false;
    if (!stop_requested_) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }
}

void MqttBackendClient::publish_presence_online() {
  Json::Value online;
  online["state"] = "online";
  online["ts"] = Json::Int64(now_unix_s());
  client_->publish(topics_.presence, json_stringify(online), /*qos*/1, /*retained*/true);
}

void MqttBackendClient::publish_status_retained() {
  Json::Value st;
  st["mode"] = "IDLE";
  st["api_ok"] = true;
  st["last_error"] = "";
  client_->publish(topics_.status, json_stringify(st), /*qos*/1, /*retained*/true);
}

void MqttBackendClient::subscribe_cmd() {
  client_->start_consuming();
  client_->subscribe(topics_.cmd, /*qos*/1)->wait();
  std::cout << "[mqtt] subscribed: " << topics_.cmd << std::endl;
}

void MqttBackendClient::publish_heartbeat_loop() {
  // simple default; actual period from app.json is parsed in main and can be passed later if needed
  const int period_ms = 1000;

  while (!stop_requested_) {
    if (st_.connected.load()) {
      Json::Value hb;
      hb["seq"] = st_.heartbeat_seq.fetch_add(1);
      hb["uptime_s"] = Json::Int64(now_steady_ms() / 1000);
      client_->publish(topics_.heartbeat, json_stringify(hb), /*qos*/0, /*retained*/false);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
  }
}

} // namespace ironsoft
