#include "mqtt_paho_client.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

static QString nowStr() {
  return QDateTime::currentDateTime().toString("HH:mm:ss");
}

static qint64 nowMs() {
  return QDateTime::currentMSecsSinceEpoch();
}

MqttPahoClient::MqttPahoClient(QObject* parent) : QObject(parent) {}

MqttPahoClient::~MqttPahoClient() {
  stop();
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
  if (prev != v) {
    QMetaObject::invokeMethod(this, [this, v]() { emit connectedChanged(v); }, Qt::QueuedConnection);
  }
}

bool MqttPahoClient::publishLoggerStart(const QString& id, const QString& sessionName) {
  QJsonObject obj;
  obj["id"] = id;
  obj["type"] = "logger.start";
  obj["ts"] = QDateTime::currentSecsSinceEpoch();
  obj["sessionName"] = sessionName;
  const QString logLine = QString("TX logger.start id=%1 session=%2").arg(id, sessionName);
  return publishCommandPayload(obj, logLine);
}

bool MqttPahoClient::publishLoggerStop(const QString& id) {
  QJsonObject obj;
  obj["id"] = id;
  obj["type"] = "logger.stop";
  obj["ts"] = QDateTime::currentSecsSinceEpoch();
  const QString logLine = QString("TX logger.stop id=%1").arg(id);
  return publishCommandPayload(obj, logLine);
}

bool MqttPahoClient::publishLoggerStatus(const QString& id) {
  QJsonObject obj;
  obj["id"] = id;
  obj["type"] = "logger.status";
  obj["ts"] = QDateTime::currentSecsSinceEpoch();
  const QString logLine = QString("TX logger.status id=%1").arg(id);
  return publishCommandPayload(obj, logLine);
}

void MqttPahoClient::setPresence(bool v) {
  const bool prev = presence_.exchange(v);
  if (prev != v) {
    QMetaObject::invokeMethod(this, [this, v]() { emit presenceChanged(v); }, Qt::QueuedConnection);
  }
}

void MqttPahoClient::connectToBroker() {
  if (worker_.joinable()) {
    emitLog(QString("[%1] already connecting").arg(nowStr()));
    return;
  }

  stop_ = false;
  setConnected(false);
  setPresence(false);
  setState(MqttConnectionState::Connecting);

  worker_ = std::thread(&MqttPahoClient::workerLoop, this);
}

void MqttPahoClient::disconnectFromBroker() {
  stop();
}

void MqttPahoClient::stop() {
  stop_ = true;
  setState(MqttConnectionState::Stopping);

  auto client = client_;
  if (client) {
    try { client->stop_consuming(); } catch (...) {}
    try {
      if (connected_.load()) client->disconnect()->wait();
    } catch (...) {}
  }

  stopCv_.notify_all();

  if (worker_.joinable()) worker_.join();
  client_.reset();

  {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    pendingPings_.clear();
  }

  setConnected(false);
  setPresence(false);
  setState(MqttConnectionState::Disconnected);
}

void MqttPahoClient::publishCmdPing() {
  // Build payload JSON like previous implementation.
  const QString pingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject obj;
  obj["id"] = pingId;
  obj["type"] = "ping";

  const auto payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);

  if (!client_ || !connected_.load()) {
    emitLog(QString("[%1] can't publish ping: not connected").arg(nowStr()));
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
    emitLog(QString("[%1] -> cmd ping id=%2").arg(nowStr(), pingId));

    {
      std::lock_guard<std::mutex> lk(pendingMutex_);
      pendingPings_.insert(pingId, nowMs());
    }

    emitPingUpdate(pingId, -1, false);
    QTimer::singleShot(2000, this, [this, pingId]() { handlePingTimeout(pingId); });
  } catch (const std::exception& e) {
    emitLog(QString("[%1] publish error: %2").arg(nowStr(), e.what()));
  }
}

bool loadFromJsonFile(const QString& path, MqttGuiConfig& outConfig, QString& outError) {
  QFile f(path);
  if (!f.exists()) {
    outError = QStringLiteral("Config file %1 not found").arg(path);
    return false;
  }
  if (!f.open(QIODevice::ReadOnly)) {
    outError = QStringLiteral("Cannot open %1: %2").arg(path, f.errorString());
    return false;
  }

  QJsonParseError parseErr{};
  const auto doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
  if (parseErr.error != QJsonParseError::NoError) {
    outError = QStringLiteral("Invalid JSON in %1: %2").arg(path, parseErr.errorString());
    return false;
  }
  if (!doc.isObject()) {
    outError = QStringLiteral("Root object must be an object in %1").arg(path);
    return false;
  }

  const auto root = doc.object();
  const auto broker = root.value("broker").toObject();
  const QString host = broker.value("host").toString();
  const int port = broker.value("port").toInt(-1);
  if (host.isEmpty()) {
    outError = QStringLiteral("Missing broker.host in %1").arg(path);
    return false;
  }
  if (port <= 0 || port > 65535) {
    outError = QStringLiteral("Invalid broker.port in %1").arg(path);
    return false;
  }

  const auto client = root.value("client").toObject();
  const QString droneId = client.value("drone_id").toString();
  if (droneId.isEmpty()) {
    outError = QStringLiteral("Missing client.drone_id in %1").arg(path);
    return false;
  }

  outConfig.host = host;
  outConfig.port = static_cast<quint16>(port);

  const auto auth = root.value("auth").toObject();
  outConfig.user = auth.value("username").toString();
  outConfig.pass = auth.value("password").toString();

  const auto tls = root.value("tls").toObject();
  outConfig.tlsEnabled = tls.value("enabled").toBool(false);
  outConfig.caFile = tls.value("ca_file").toString();

  outConfig.droneId = droneId;
  int keepalive = client.value("keepalive_s").toInt(outConfig.keepalive_s);
  if (keepalive <= 0) keepalive = 20;
  outConfig.keepalive_s = keepalive;

  return true;
}

void MqttPahoClient::setState(MqttConnectionState state) {
  const auto prev = state_.exchange(state);
  if (prev == state) return;
  QMetaObject::invokeMethod(this, [this, state]() { emit stateChanged(state); }, Qt::QueuedConnection);
}

void MqttPahoClient::emitLog(const QString& line) {
  QMetaObject::invokeMethod(this, [this, line]() { emit logLine(line); }, Qt::QueuedConnection);
}

void MqttPahoClient::emitHeartbeat() {
  QMetaObject::invokeMethod(this, [this]() { emit heartbeatReceived(); }, Qt::QueuedConnection);
}

void MqttPahoClient::emitPingUpdate(const QString& id, qint64 rttMs, bool timeout) {
  QMetaObject::invokeMethod(
      this,
      [this, id, rttMs, timeout]() { emit pingUpdated(id, rttMs, timeout); },
      Qt::QueuedConnection);
}

void MqttPahoClient::emitAckEvent(const QString& type, const QString& id, bool ok) {
  QMetaObject::invokeMethod(
      this,
      [this, type, id, ok]() { emit ackReceived(type, id, ok); },
      Qt::QueuedConnection);
}

void MqttPahoClient::handlePingTimeout(const QString& id) {
  bool removed = false;
  {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    auto it = pendingPings_.find(id);
    if (it != pendingPings_.end()) {
      pendingPings_.erase(it);
      removed = true;
    }
  }

  if (!removed || stop_.load()) return;

  emitLog(QString("[%1] ping timeout (id=%2)").arg(nowStr(), id));
  emitPingUpdate(id, -1, true);
}

bool MqttPahoClient::parseStatusJson(const QByteArray& payload, BackendStatus& out, QString& err) {
  err.clear();
  QJsonParseError parseErr{};
  const auto doc = QJsonDocument::fromJson(payload, &parseErr);
  if (parseErr.error != QJsonParseError::NoError) {
    err = parseErr.errorString();
    return false;
  }
  if (!doc.isObject()) {
    err = QStringLiteral("root is not an object");
    return false;
  }

  const auto obj = doc.object();

  if (obj.contains("mode")) {
    const auto modeVal = obj.value("mode");
    if (modeVal.isString()) out.mode = modeVal.toString();
    else emitLog(QStringLiteral("[status] missing field 'mode'"));
  } else {
    emitLog(QStringLiteral("[status] missing field 'mode'"));
  }

  if (obj.contains("api_ok")) {
    const auto apiVal = obj.value("api_ok");
    if (apiVal.isBool()) out.api_ok = apiVal.toBool();
    else emitLog(QStringLiteral("[status] missing field 'api_ok'"));
  } else {
    emitLog(QStringLiteral("[status] missing field 'api_ok'"));
  }

  if (obj.contains("last_error")) {
    const auto errVal = obj.value("last_error");
    if (errVal.isString()) out.last_error = errVal.toString();
    else emitLog(QStringLiteral("[status] missing field 'last_error'"));
  } else {
    emitLog(QStringLiteral("[status] missing field 'last_error'"));
  }

  const auto tsVal = obj.value("ts");
  if (tsVal.isDouble()) out.ts = static_cast<qint64>(tsVal.toDouble());
  else out.ts = QDateTime::currentSecsSinceEpoch();

  if (obj.contains("recording_active")) {
    const auto recVal = obj.value("recording_active");
    if (recVal.isBool()) out.recording_active = recVal.toBool();
  } else if (obj.contains("recording")) {
    const auto recVal = obj.value("recording");
    if (recVal.isBool()) out.recording_active = recVal.toBool();
  }

  const QJsonValue sessionField = obj.contains("session_name") ? obj.value("session_name") : obj.value("sessionName");
  if (sessionField.isString()) {
    out.session_name = sessionField.toString();
  } else {
    out.session_name.clear();
  }

  return true;
}

bool MqttPahoClient::publishCommandPayload(const QJsonObject& obj, const QString& logLine) {
  if (!client_ || !connected_.load()) {
    emitLog(QString("[%1] can't publish %2: not connected")
                .arg(nowStr(), obj.value("type").toString("cmd")));
    return false;
  }

  const auto payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  try {
    auto m = mqtt::make_message(tCmd().toStdString(),
                                std::string(payload.constData(), payload.size()));
    m->set_qos(1);
    m->set_retained(false);
    mqtt::const_message_ptr cm = m;
    client_->publish(cm);
    emitLog(QString("[%1] %2").arg(nowStr(), logLine));
    return true;
  } catch (const std::exception& e) {
    emitLog(QString("[%1] publish error: %2").arg(nowStr(), e.what()));
    return false;
  }
}

void MqttPahoClient::workerLoop() {
  int backoffMs = 1000;
  std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

  while (!stop_) {
    MqttGuiConfig cfg;
    {
      std::lock_guard<std::mutex> lk(cfgMutex_);
      cfg = cfg_;
    }

    const std::string serverURI = ("tcp://" + cfg.host + ":" + QString::number(cfg.port)).toStdString();
    const std::string clientId = ("ironsoft-gui-" + cfg.droneId).toStdString();
    client_ = std::make_shared<mqtt::async_client>(serverURI, clientId);

    try {
      setState(MqttConnectionState::Connecting);
      mqtt::connect_options_builder b;
      b.clean_session(true);
      b.keep_alive_interval(std::chrono::seconds(std::max(1, cfg.keepalive_s)));

      if (!cfg.user.isEmpty()) {
        b.user_name(cfg.user.toStdString());
        b.password(cfg.pass.toStdString());
      }

      auto connOpts = b.finalize();
      emitLog(QString("[%1] connecting to %2:%3").arg(nowStr(), cfg.host).arg(cfg.port));
      client_->connect(connOpts)->wait();
      setConnected(true);
      setState(MqttConnectionState::Connected);
      emitLog(QString("[%1] connected").arg(nowStr()));

      client_->subscribe(tPresence().toStdString(), 1)->wait();
      client_->subscribe(tStatus().toStdString(), 1)->wait();
      client_->subscribe(tHeartbeat().toStdString(), 0)->wait();
      client_->subscribe(tAck().toStdString(), 1)->wait();
      emitLog(QString("[%1] subscribed presence/status/heartbeat/ack").arg(nowStr()));

      client_->start_consuming();
      backoffMs = 1000;

      while (!stop_) {
        auto msg = client_->consume_message();
        if (!msg) {
          if (!stop_) {
            emitLog(QString("[%1] consume returned null (disconnected)").arg(nowStr()));
          }
          break;
        }

        const QString topic = QString::fromStdString(msg->get_topic());
        const QString payload = QString::fromStdString(msg->to_string());

        emitLog(QString("[%1] <- %2 %3").arg(nowStr(), topic, payload));

        if (topic == tPresence()) {
          QJsonParseError err{};
          const auto doc = QJsonDocument::fromJson(payload.toUtf8(), &err);
          if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const auto obj = doc.object();
            const auto state = obj.value("state").toString();
            if (state == "online") setPresence(true);
            else if (state == "offline") setPresence(false);
          }
        } else if (topic == tStatus()) {
          BackendStatus st;
          QString statusErr;
          if (parseStatusJson(payload.toUtf8(), st, statusErr)) {
            emitLog(QString("[%1] <- status mode=%2 api_ok=%3 err='%4'")
                        .arg(nowStr(), st.mode.isEmpty() ? "?" : st.mode)
                        .arg(st.api_ok ? "1" : "0")
                        .arg(st.last_error));
            emit statusChanged(st);
          } else {
            emitLog(QString("[%1] [status] BAD_JSON: %2").arg(nowStr(), statusErr));
          }
        } else if (topic == tHeartbeat()) {
          emitHeartbeat();
        } else if (topic == tAck()) {
          QJsonParseError ackErr{};
          const auto doc = QJsonDocument::fromJson(payload.toUtf8(), &ackErr);
          if (ackErr.error != QJsonParseError::NoError || !doc.isObject()) {
            emitLog(QString("[%1] [ack] BAD_JSON: %2").arg(nowStr(), ackErr.errorString()));
            continue;
          }

          const auto ackObj = doc.object();
          const QString ackId = ackObj.value("id").toString();
          const bool ok = ackObj.value("ok").toBool(false);
          const QString message = ackObj.value("message").toString();
          const QString errText = ackObj.value("err").toString(ackObj.value("error").toString());
          const QString ackType = ackObj.value("type").toString();
          const int httpCode = ackObj.value("http_code").toInt(-1);

          if (ackId.isEmpty()) {
            emitLog(QString("[%1] [ack] missing id").arg(nowStr()));
            continue;
          }

          qint64 sentMs = 0;
          bool matchedPing = false;
          {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            auto it = pendingPings_.find(ackId);
            if (it != pendingPings_.end()) {
              sentMs = it.value();
              pendingPings_.erase(it);
              matchedPing = true;
            }
          }

          if (matchedPing) {
            const qint64 rtt = std::max<qint64>(0, nowMs() - sentMs);
            emitLog(QString("[%1] pong id=%2 (%3 ms)").arg(nowStr(), ackId).arg(rtt));
            emitPingUpdate(ackId, rtt, false);
            emitAckEvent(ackType.isEmpty() ? QStringLiteral("ping") : ackType, ackId, ok);
            continue;
          }

          emitLog(QString("[%1] RX ack type=%2 id=%3 ok=%4 http=%5 msg='%6' err='%7'")
                      .arg(nowStr())
                      .arg(ackType.isEmpty() ? QStringLiteral("-") : ackType)
                      .arg(ackId)
                      .arg(ok ? QStringLiteral("1") : QStringLiteral("0"))
                      .arg(httpCode)
                      .arg(message.isEmpty() ? QStringLiteral("-") : message)
                      .arg(errText.isEmpty() ? QStringLiteral("-") : errText));
          emitAckEvent(ackType, ackId, ok);
        }
      }

      try { client_->stop_consuming(); } catch (...) {}
      try {
        if (connected_.load()) client_->disconnect()->wait();
      } catch (...) {}
      setConnected(false);
      setPresence(false);

    } catch (const std::exception& e) {
      setConnected(false);
      setPresence(false);
      emitLog(QString("[%1] mqtt error: %2").arg(nowStr(), e.what()));
    }

    if (stop_) break;

    setState(MqttConnectionState::Disconnected);

    int jitter = backoffMs / 5;
    std::uniform_int_distribution<int> dist(-jitter, jitter);
    int delay = backoffMs + (jitter > 0 ? dist(rng) : 0);
    delay = std::clamp(delay, 200, 10000);
    emitLog(QString("[%1] reconnect in %2 ms").arg(nowStr()).arg(delay));

    std::unique_lock<std::mutex> lk(stopMutex_);
    stopCv_.wait_for(lk, std::chrono::milliseconds(delay), [this]() { return stop_.load(); });

    if (stop_) break;

    backoffMs = std::min(backoffMs * 2, 10000);
  }

  setConnected(false);
  setPresence(false);
  client_.reset();
  setState(stop_ ? MqttConnectionState::Stopping : MqttConnectionState::Disconnected);
}
