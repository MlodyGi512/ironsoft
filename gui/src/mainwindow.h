#pragma once
#include <QMainWindow>
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
  void loadTheme();
  void loadConfigDefaults();
  void setLedOnline(bool online);

private slots:
  void onConnectClicked();
  void onDisconnectClicked();
  void onPingClicked();

  void onLogLine(const QString& line);
  void onConnectedChanged(bool connected);
  void onPresenceChanged(bool online);

private:
  Ui::MainWindow* ui;
  MqttPahoClient mqtt_;
};
