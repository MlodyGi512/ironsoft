#include "mqtt_paho_client.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <chrono>
#include <thread>

static QString nowStr() {
  return QDateTime::currentDateTime().toString("HH:mm:ss");
}

MqttPahoClient::MqttPahoClient(QObject* parent) : QObject(parent) {}

MqttPahoClient::~MqttPahoClient() {
  disconnectFromBroker();
}

void MqttPahoClient::setConfig(const MqttGuiConfig& cfg) {
  std::lock_guard<std::mutex> lk(cfgMutex_);
  cfg_ = cfg;
}

QString MqttPahoClient::topicPrefix() const {
  // copy under lock
  std::lock_guard<std::mutex> lk(cfgMutex_);
  return QString("ironsoft/uav/%1").arg(cfg_.droneId);
}

void MqttPahoClient::setConnected(bool v) {
  const bool prev = connected_.exchange(v);
  if (prev != v) emit connectedChanged(v);
}

void MqttPahoClient::setPresence(bool v) {
  const bool prev = presence_.exchange(v);
  if (prev != v) emit presenceChanged(v);
}

void MqttPahoClient::connectToBroker() {
  // If already running, ignore
  if (worker_.joinable()) {
    emit logLine(QString("[%1] already running").arg(nowStr()));
    return;
  }

  stop_ = false;
  setConnected(false);
  setPresence(false);

  worker_ = std::thread(&MqttPahoClient::workerLoop, this);
}

void MqttPahoClient::disconnectFromBroker() {
  stop_ = true;

  try {
    if (client_) {
      try { client_->stop_consuming(); } catch (...) {}
      try {
        if (connected_.load()) client_->disconnect()->wait();
      } catch (...) {}
    }
  } catch (...) {}

  if (worker_.joinable()) worker_.join();
  client_.reset();

  setConnected(false);
  setPresence(false);
}

void MqttPahoClient::publishCmdPing() {
  // Build payload JSON like previous implementation.
  QJsonObject obj;
  obj["id"] = QString("cmd-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  obj["type"] = "ping";
  obj["params"] = QJsonObject{};

  const auto payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);

  if (!client_ || !connected_.load()) {
    emit logLine(QString("[%1] can't publish ping: not connected").arg(nowStr()));
    return;
  }

  try {
    // publish() expects const_message_ptr on this Paho version; convert explicitly.
    auto m = mqtt::make_message(tCmd().toStdString(),
                               std::string(payload.constData(), payload.size()));
    m->set_qos(1);
    m->set_retained(false);
    mqtt::const_message_ptr cm = m;
    client_->publish(cm);
    emit logLine(QString("[%1] -> cmd ping").arg(nowStr()));
  } catch (const std::exception& e) {
    emit logLine(QString("[%1] publish error: %2").arg(nowStr(), e.what()));
  }
}

void MqttPahoClient::workerLoop() {
  // Copy config snapshot
  MqttGuiConfig cfg;
  {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    cfg = cfg_;
  }

  const std::string serverURI = ("tcp://" + cfg.host + ":" + QString::number(cfg.port)).toStdString();
  const std::string clientId = ("ironsoft-gui-" + cfg.droneId).toStdString();

  client_ = std::make_unique<mqtt::async_client>(serverURI, clientId);

  int backoffMs = 500;

  while (!stop_) {
    try {
      mqtt::connect_options_builder b;
      b.clean_session(true);
      b.keep_alive_interval(std::chrono::seconds(20));

      if (!cfg.user.isEmpty()) {
        b.user_name(cfg.user.toStdString());
        b.password(cfg.pass.toStdString());
      }

      // No LWT for GUI client; presence comes from backend.
      auto connOpts = b.finalize();

      emit logLine(QString("[%1] connecting to %2").arg(nowStr(), cfg.host));
      client_->connect(connOpts)->wait();
      setConnected(true);
      emit logLine(QString("[%1] connected").arg(nowStr()));

      // Subscribe to topics
      client_->subscribe(tPresence().toStdString(), 1)->wait();
      client_->subscribe(tStatus().toStdString(), 1)->wait();
      client_->subscribe(tHeartbeat().toStdString(), 0)->wait();
      client_->subscribe(tAck().toStdString(), 1)->wait();
      emit logLine(QString("[%1] subscribed presence/status/heartbeat/ack").arg(nowStr()));

      // Start consuming
      client_->start_consuming();

      // Reset backoff after successful connection
      backoffMs = 500;

      while (!stop_) {
        // Blocking consume; returns nullptr on shutdown/disconnect.
        auto msg = client_->consume_message();
        if (!msg) {
          if (!stop_) {
            emit logLine(QString("[%1] consume returned null (disconnected)").arg(nowStr()));
          }
          break;
        }

        const QString topic = QString::fromStdString(msg->get_topic());
        const QString payload = QString::fromStdString(msg->to_string());

        emit logLine(QString("[%1] <- %2 %3").arg(nowStr(), topic, payload));

        if (topic == tPresence()) {
          // Parse minimal presence JSON: {"state":"online"|"offline", ...}
          QJsonParseError err{};
          const auto doc = QJsonDocument::fromJson(payload.toUtf8(), &err);
          if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const auto obj = doc.object();
            const auto state = obj.value("state").toString();
            if (state == "online") setPresence(true);
            else if (state == "offline") setPresence(false);
          }
        }
      }

      // Clean disconnect
      try { client_->stop_consuming(); } catch (...) {}
      try {
        if (connected_.load()) client_->disconnect()->wait();
      } catch (...) {}
      setConnected(false);
      setPresence(false);

    } catch (const std::exception& e) {
      setConnected(false);
      setPresence(false);
      emit logLine(QString("[%1] mqtt error: %2").arg(nowStr(), e.what()));
    }

    if (stop_) break;

    // Backoff before reconnect
    emit logLine(QString("[%1] reconnect in %2 ms").arg(nowStr()).arg(backoffMs));
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    backoffMs = std::min(backoffMs * 2, 5000);
  }
}
