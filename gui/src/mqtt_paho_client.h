#pragma once
#include <QObject>
#include <atomic>
#include <thread>
#include <mutex>

#include <mqtt/async_client.h>

struct MqttGuiConfig {
  QString host;
  quint16 port = 1883;
  QString user;
  QString pass;
  QString droneId = "drone01";
};

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
  Q_INVOKABLE void publishCmdPing();

signals:
  void logLine(const QString& line);
  void connectedChanged(bool connected);
  void presenceChanged(bool online);

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

private:
  MqttGuiConfig cfg_;
  mutable std::mutex cfgMutex_;

  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> presence_{false};

  std::unique_ptr<mqtt::async_client> client_;
  std::thread worker_;
};
