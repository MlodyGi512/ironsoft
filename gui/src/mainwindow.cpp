#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

static QString readFileText(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(f.readAll());
}

MainWindow::MainWindow(QWidget* parent)
  : QMainWindow(parent)
  , ui(new Ui::MainWindow) {
  ui->setupUi(this);

  loadTheme();
  loadConfigDefaults();
  setLedOnline(false);

  connect(ui->btnConnect, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
  connect(ui->btnDisconnect, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
  connect(ui->btnPing, &QPushButton::clicked, this, &MainWindow::onPingClicked);

  connect(&mqtt_, &MqttPahoClient::logLine, this, &MainWindow::onLogLine);
  connect(&mqtt_, &MqttPahoClient::connectedChanged, this, &MainWindow::onConnectedChanged);
  connect(&mqtt_, &MqttPahoClient::presenceChanged, this, &MainWindow::onPresenceChanged);
}

MainWindow::~MainWindow() {
  delete ui;
}

void MainWindow::loadTheme() {
  const QString qss = readFileText(":/theme.qss");
  if (!qss.isEmpty()) {
    qApp->setStyleSheet(qss);
    return;
  }
  // fallback: load from disk (dev run)
  const QString disk = readFileText("src/theme.qss");
  if (!disk.isEmpty()) qApp->setStyleSheet(disk);
}

void MainWindow::loadConfigDefaults() {
  // dev: load ../config/mqtt.dev.json if present (when running from build dir you can copy it near exe)
  // If missing, keep placeholders.
  const QString jsonText = readFileText("mqtt.dev.json");
  if (jsonText.isEmpty()) {
    // default to your current broker IP for convenience; user can edit in UI
    ui->editHost->setText("192.168.1.36");
    ui->spinPort->setValue(1883);
    ui->editDroneId->setText("drone01");
    return;
  }

  const auto doc = QJsonDocument::fromJson(jsonText.toUtf8());
  if (!doc.isObject()) return;
  const auto root = doc.object();

  const auto broker = root.value("broker").toObject();
  ui->editHost->setText(broker.value("host").toString("192.168.1.36"));
  ui->spinPort->setValue(broker.value("port").toInt(1883));

  const auto auth = root.value("auth").toObject();
  ui->editUser->setText(auth.value("username").toString(""));
  ui->editPass->setText(auth.value("password").toString(""));

  const auto client = root.value("client").toObject();
  ui->editDroneId->setText(client.value("drone_id").toString("drone01"));
}

void MainWindow::setLedOnline(bool online) {
  if (online) {
    ui->led->setStyleSheet("QLabel#led{background-color:#1B5A2B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}");
  } else {
    ui->led->setStyleSheet("QLabel#led{background-color:#5A1B1B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}");
  }
}

void MainWindow::onConnectClicked() {
  MqttGuiConfig cfg;
  cfg.host = ui->editHost->text().trimmed();
  cfg.port = static_cast<quint16>(ui->spinPort->value());
  cfg.user = ui->editUser->text();
  cfg.pass = ui->editPass->text();
  cfg.droneId = ui->editDroneId->text().trimmed();

  mqtt_.setConfig(cfg);
  mqtt_.connectToBroker();
}

void MainWindow::onDisconnectClicked() {
  mqtt_.disconnectFromBroker();
}

void MainWindow::onPingClicked() {
  mqtt_.publishCmdPing();
}

void MainWindow::onLogLine(const QString& line) {
  ui->textLog->append(line);
}

void MainWindow::onConnectedChanged(bool connected) {
  ui->labelConnState->setText(connected ? "Connected" : "Disconnected");
}

void MainWindow::onPresenceChanged(bool online) {
  setLedOnline(online);
}
