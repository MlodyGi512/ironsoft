#pragma once
#include <QObject>
#include <QMetaType>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>
#include <mutex>

#include <mqtt/async_client.h>

struct MqttGuiConfig {
  QString host;
  quint16 port = 1883;
  QString user;
  QString pass;
  QString droneId = "drone01";
  bool tlsEnabled = false;
  QString caFile;
  int keepalive_s = 20;
};

bool loadFromJsonFile(const QString& path, MqttGuiConfig& outConfig, QString& outError);

enum class MqttConnectionState { Disconnected, Connecting, Connected, Stopping };
Q_DECLARE_METATYPE(MqttConnectionState)

struct BackendStatus {
  QString mode;
  bool api_ok = false;
  QString last_error;
  qint64 ts = 0;
};
Q_DECLARE_METATYPE(BackendStatus)

// Minimal MQTT client for Qt GUI using Paho MQTT C++ (async_client).
// Runs the MQTT loop in a dedicated std::thread and communicates with UI via Qt signals.
class MqttPahoClient : public QObject {
  Q_OBJECT
public:
  explicit MqttPahoClient(QObject* parent = nullptr);
  ~MqttPahoClient() override;

  void setConfig(const MqttGuiConfig& cfg);

  Q_INVOKABLE void connectToBroker();
  Q_INVOKABLE void disconnectFromBroker();
  Q_INVOKABLE void stop();
  Q_INVOKABLE void publishCmdPing();

signals:
  void logLine(const QString& line);
  void connectedChanged(bool connected);
  void presenceChanged(bool online);
  void stateChanged(MqttConnectionState state);
  void heartbeatReceived();
  void statusChanged(const BackendStatus& st);
  void pingUpdated(const QString& id, qint64 rttMs, bool timeout);

private:
  QString topicPrefix() const;
  QString tPresence() const { return topicPrefix() + "/presence"; }
  QString tStatus() const { return topicPrefix() + "/status"; }
  QString tHeartbeat() const { return topicPrefix() + "/heartbeat"; }
  QString tAck() const { return topicPrefix() + "/ack"; }
  QString tCmd() const { return topicPrefix() + "/cmd"; }

  void workerLoop();
  void setConnected(bool v);
  void setPresence(bool v);
  void setState(MqttConnectionState state);
  void emitLog(const QString& line);
  void emitHeartbeat();
  void emitPingUpdate(const QString& id, qint64 rttMs, bool timeout);
  void handlePingTimeout(const QString& id);
  bool parseStatusJson(const QByteArray& payload, BackendStatus& out, QString& err);

private:
  MqttGuiConfig cfg_;
  mutable std::mutex cfgMutex_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> presence_{false};
  std::atomic<MqttConnectionState> state_{MqttConnectionState::Disconnected};

  std::shared_ptr<mqtt::async_client> client_;
  std::thread worker_;
  std::condition_variable stopCv_;
  std::mutex stopMutex_;

  QHash<QString, qint64> pendingPings_;
  std::mutex pendingMutex_;
};
