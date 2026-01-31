#pragma once
#include <QMainWindow>
#include <QTimer>
#include "mqtt_paho_client.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow();

private:
  enum class BackendHealth { Offline, Stale, Online };

  void loadTheme();
  void loadConfigDefaults();
  void setPresenceLed(bool online);
  void setBackendLed(BackendHealth state);
  void setLedApi(bool ok);
  void setModeText(const QString& mode);
  void setLastError(const QString& err);
  void setPingRttDisplay(qint64 rttMs, bool timeout);
  MqttGuiConfig defaultConfig() const;
  void applyConfigToUi(const MqttGuiConfig& cfg);
  bool loadConfigFromPath(const QString& path, bool showDialogs);
  void saveConfigPath(const QString& path);
  void logUi(const QString& line);
  void updateUiForState(MqttConnectionState state);
  void updateBackendHealth();

private slots:
  void onConnectClicked();
  void onPingClicked();
  void onBrowseConfigClicked();

  void onLogLine(const QString& line);
  void onConnectedChanged(bool connected);
  void onPresenceChanged(bool online);
  void onClientStateChanged(MqttConnectionState state);
  void onHeartbeatReceived();
  void onHeartbeatTick();
  void onStatusChanged(const BackendStatus& st);
  void onPingUpdated(const QString& id, qint64 rttMs, bool timeout);

private:
  Ui::MainWindow* ui;
  MqttPahoClient mqtt_;
  MqttGuiConfig currentConfig_;
  MqttConnectionState clientState_ = MqttConnectionState::Disconnected;
  BackendHealth backendHealth_ = BackendHealth::Offline;
  QTimer heartbeatTimer_;
  bool presenceOnline_ = false;
  qint64 lastHeartbeatMs_ = 0;
  static constexpr int kHeartbeatTimeoutMs = 3000;
};
