#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMetaType>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

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
  currentConfig_ = defaultConfig();
  loadConfigDefaults();
  setPresenceLed(false);
  setBackendLed(BackendHealth::Offline);
  setLedApi(false);
  setModeText("—");
  setLastError({});
  setPingRttDisplay(-1, false);
  qRegisterMetaType<MqttConnectionState>("MqttConnectionState");
  qRegisterMetaType<BackendStatus>("BackendStatus");
  qRegisterMetaType<EkinoxStatus>("EkinoxStatus");

  connect(ui->btnConnect, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
  connect(ui->btnPing, &QPushButton::clicked, this, &MainWindow::onPingClicked);
  connect(ui->btnBrowseConfig, &QPushButton::clicked, this, &MainWindow::onBrowseConfigClicked);
  connect(ui->btnLoggerStart, &QPushButton::clicked, this, &MainWindow::onLoggerStartClicked);
  connect(ui->btnLoggerStop, &QPushButton::clicked, this, &MainWindow::onLoggerStopClicked);

  connect(&mqtt_, &MqttPahoClient::logLine, this, &MainWindow::onLogLine);
  connect(&mqtt_, &MqttPahoClient::connectedChanged, this, &MainWindow::onConnectedChanged);
  connect(&mqtt_, &MqttPahoClient::presenceChanged, this, &MainWindow::onPresenceChanged);
  connect(&mqtt_, &MqttPahoClient::stateChanged, this, &MainWindow::onClientStateChanged);
  connect(&mqtt_, &MqttPahoClient::heartbeatReceived, this, &MainWindow::onHeartbeatReceived);
  connect(&mqtt_, &MqttPahoClient::statusChanged, this, &MainWindow::onStatusChanged);
  connect(&mqtt_, &MqttPahoClient::pingUpdated, this, &MainWindow::onPingUpdated);
  connect(&mqtt_, &MqttPahoClient::ackReceived, this, &MainWindow::onAckReceived);
  connect(&mqtt_, &MqttPahoClient::ekinoxPresenceChanged, this, &MainWindow::onEkinoxPresenceChanged);
  connect(&mqtt_, &MqttPahoClient::ekinoxStatusChanged, this, &MainWindow::onEkinoxStatusChanged);
  connect(&mqtt_, &MqttPahoClient::ekinoxHeartbeatReceived, this, &MainWindow::onEkinoxHeartbeatReceived);

  updateUiForState(clientState_);
  ui->btnPing->setEnabled(false);
  updateLoggerControls();

  heartbeatTimer_.setInterval(500);
  heartbeatTimer_.setSingleShot(false);
  connect(&heartbeatTimer_, &QTimer::timeout, this, &MainWindow::onHeartbeatTick);
  heartbeatTimer_.start();
}
MqttGuiConfig MainWindow::defaultConfig() const {
  MqttGuiConfig cfg;
  cfg.host = "192.168.1.36";
  cfg.port = 1883;
  cfg.user.clear();
  cfg.pass.clear();
  cfg.droneId = "drone01";
  cfg.tlsEnabled = false;
  cfg.caFile.clear();
  cfg.keepalive_s = 20;
  return cfg;
}

void MainWindow::applyConfigToUi(const MqttGuiConfig& cfg) {
  currentConfig_ = cfg;
  ui->editHost->setText(cfg.host);
  ui->spinPort->setValue(cfg.port);
  ui->editUser->setText(cfg.user);
  ui->editPass->setText(cfg.pass);
  ui->editDroneId->setText(cfg.droneId);
}

bool MainWindow::loadConfigFromPath(const QString& path, bool showDialogs) {
  if (path.isEmpty()) {
    applyConfigToUi(defaultConfig());
    ui->editConfigPath->clear();
    logUi("[cfg] No config path provided, using defaults");
    return false;
  }

  QString err;
  MqttGuiConfig cfg;
  QFileInfo info(path);
  const QString prettyPath = QDir::toNativeSeparators(info.absoluteFilePath());
  const bool exists = info.exists();

  if (!loadFromJsonFile(path, cfg, err)) {
    applyConfigToUi(defaultConfig());
    ui->editConfigPath->setText(prettyPath);
    logUi("[cfg] " + err + ", using defaults");
    if (exists && showDialogs) {
      QMessageBox::warning(this, tr("Config error"), tr("Cannot load config:\n%1").arg(err));
    }
    return false;
  }

  applyConfigToUi(cfg);
  ui->editConfigPath->setText(prettyPath);
  logUi("[cfg] loaded: " + prettyPath);
  return true;
}

void MainWindow::saveConfigPath(const QString& path) {
  if (path.isEmpty()) return;
  QSettings settings("IronSoft", "GCS");
  settings.setValue("mqtt/config_path", QFileInfo(path).absoluteFilePath());
}

void MainWindow::logUi(const QString& line) {
    ui->textLog->append(line);
}

void MainWindow::logUiState(const QString& tag) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 heartbeatAge = (lastHeartbeatMs_ > 0) ? (now - lastHeartbeatMs_) : -1;
  const QString mode = hasStatus_ ? (lastStatus_.mode.isEmpty() ? QStringLiteral("<empty>") : lastStatus_.mode) : QStringLiteral("<none>");
  const QString api = hasStatus_ ? (lastStatus_.api_ok ? QStringLiteral("true") : QStringLiteral("false")) : QStringLiteral("<n/a>");
  const QString lastErr = hasStatus_ ? (lastStatus_.last_error.isEmpty() ? QStringLiteral("<empty>") : lastStatus_.last_error) : QStringLiteral("<n/a>");
  logUi(QStringLiteral("[ui-state][%1] mqttConnected=%2 presenceOnline=%3 heartbeatAgeMs=%4 hasStatus=%5 mode=%6 api_ok=%7 last_error=%8")
            .arg(tag)
            .arg(clientState_ == MqttConnectionState::Connected ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(presenceOnline_ ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(heartbeatAge)
            .arg(hasStatus_ ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(mode)
            .arg(api)
            .arg(lastErr));
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
  QSettings settings("IronSoft", "GCS");
  const QString defaultPath = QDir(QCoreApplication::applicationDirPath()).filePath("mqtt.dev.json");
  if (!settings.contains("mqtt/config_path")) {
    settings.setValue("mqtt/config_path", defaultPath);
  }

  const QString storedPath = settings.value("mqtt/config_path", defaultPath).toString();
  ui->editConfigPath->setText(QDir::toNativeSeparators(storedPath));

  loadConfigFromPath(storedPath, false);
}

void MainWindow::setPresenceLed(bool online) {
  const QString style = online
      ? "QLabel#ledPresence{background-color:#1B5A2B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}"
      : "QLabel#ledPresence{background-color:#5A1B1B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}";
  ui->ledPresence->setStyleSheet(style);
}

void MainWindow::setBackendLed(BackendHealth state) {
  backendHealth_ = state;
  QString style;
  QString tooltip;
  switch (state) {
    case BackendHealth::Online:
      style = "QLabel#ledBackend{background-color:#1B5A2B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}";
      tooltip = tr("Backend online");
      break;
    case BackendHealth::Stale:
      style = "QLabel#ledBackend{background-color:#B38F00;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}";
      tooltip = tr("Backend heartbeat stale");
      break;
    case BackendHealth::Offline:
    default:
      style = "QLabel#ledBackend{background-color:#5A1B1B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}";
      tooltip = tr("Backend offline");
      break;
  }
  ui->ledBackend->setStyleSheet(style);
  ui->ledBackend->setToolTip(tooltip);
}

void MainWindow::setLedApi(bool ok) {
  const QString style = ok
      ? "QLabel#ledApi{background-color:#1B5A2B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}"
      : "QLabel#ledApi{background-color:#5A1B1B;border:2px solid #D4AF37;border-radius:8px;min-width:16px;min-height:16px;max-width:16px;max-height:16px;}";
  ui->ledApi->setStyleSheet(style);
  ui->ledApi->setToolTip(ok ? tr("API healthy") : tr("API offline"));
}

void MainWindow::setModeText(const QString& mode) {
  ui->labelModeValue->setText(mode.isEmpty() ? QStringLiteral("—") : mode);
}

void MainWindow::setLastError(const QString& err) {
  const bool hasError = !err.trimmed().isEmpty();
  ui->labelErrorValue->setWordWrap(true);
  ui->labelErrorValue->setText(hasError ? err : QStringLiteral("—"));
  ui->iconError->setVisible(hasError);
}

void MainWindow::setPingRttDisplay(qint64 rttMs, bool timeout) {
  QString text;
  QString color;
  if (timeout) {
    text = tr("timeout");
    color = "#B34700";
  } else if (rttMs < 0) {
    text = tr("…");
    color = "#888888";
  } else {
    text = tr("%1 ms").arg(rttMs);
    color = "#1B5A2B";
  }

  ui->labelPingRttValue->setText(text);
  ui->labelPingRttValue->setStyleSheet(QStringLiteral("QLabel#labelPingRttValue{color:%1;font-weight:bold;}").arg(color));
}

void MainWindow::onConnectClicked() {
  if (clientState_ == MqttConnectionState::Stopping) {
    logUi("[mqtt] stop in progress...");
    return;
  }

  if (clientState_ == MqttConnectionState::Disconnected) {
    MqttGuiConfig cfg = currentConfig_;
    cfg.host = ui->editHost->text().trimmed();
    cfg.port = static_cast<quint16>(ui->spinPort->value());
    cfg.user = ui->editUser->text();
    cfg.pass = ui->editPass->text();
    cfg.droneId = ui->editDroneId->text().trimmed();

    currentConfig_ = cfg;
    mqtt_.setConfig(cfg);
    mqtt_.connectToBroker();
    QTimer::singleShot(1000, this, [this]() { logUiState(QStringLiteral("afterConnect")); });
  } else {
    mqtt_.stop();
  }
}

void MainWindow::onPingClicked() {
  if (clientState_ != MqttConnectionState::Connected) {
    logUi("[mqtt] cannot send ping while disconnected");
    return;
  }
  mqtt_.publishCmdPing();
}

void MainWindow::onBrowseConfigClicked() {
  QString startPath = ui->editConfigPath->text();
  if (startPath.isEmpty()) {
    startPath = QCoreApplication::applicationDirPath();
  }
  QFileInfo startInfo(startPath);
  const QString initialDir = startInfo.isDir() ? startInfo.absoluteFilePath() : startInfo.absolutePath();

  const QString path = QFileDialog::getOpenFileName(this, tr("Select MQTT config"), initialDir, tr("JSON files (*.json);;All files (*)"));
  if (path.isEmpty()) return;

  if (loadConfigFromPath(path, true)) {
    saveConfigPath(path);
  }
}

void MainWindow::onLogLine(const QString& line) {
  logUi(line);
}

void MainWindow::onConnectedChanged(bool connected) {
  ui->btnPing->setEnabled(connected);
  if (!connected) {
    presenceOnline_ = false;
    lastHeartbeatMs_ = 0;
    setPresenceLed(false);
    ekinoxPresenceOnline_ = false;
    lastEkinoxHeartbeatMs_ = 0;
  }
  updateBackendHealth();
  updateLoggerControls();
}

void MainWindow::onPresenceChanged(bool online) {
  presenceOnline_ = online;
  if (!online) {
    lastHeartbeatMs_ = 0;
  }
  logUiState(QStringLiteral("afterPresence"));
}

void MainWindow::onClientStateChanged(MqttConnectionState state) {
  clientState_ = state;
  updateUiForState(state);
  if (state != MqttConnectionState::Connected) {
    lastHeartbeatMs_ = 0;
  }
  updateBackendHealth();
  updateLoggerControls();
}

void MainWindow::onHeartbeatReceived() {
  lastHeartbeatMs_ = QDateTime::currentMSecsSinceEpoch();
  updateBackendHealth();
  logUiState(QStringLiteral("afterHeartbeat"));
}

void MainWindow::onHeartbeatTick() {
  updateBackendHealth();
}

void MainWindow::onStatusChanged(const BackendStatus& st) {
  setModeText(st.mode);
  setLedApi(st.api_ok);
  setLastError(st.last_error);
  lastStatus_ = st;
  hasStatus_ = true;
  updateLoggerControls();
  logUiState(QStringLiteral("afterStatus"));
}

void MainWindow::onPingUpdated(const QString& /*id*/, qint64 rttMs, bool timeout) {
  setPingRttDisplay(rttMs, timeout);
}

void MainWindow::onAckReceived(const QString& type, const QString& id, bool ok) {
  const QString prettyType = type.isEmpty() ? QStringLiteral("-") : type;
  const QString line = tr("[ack] type=%1 id=%2 ok=%3").arg(prettyType, id.isEmpty() ? QStringLiteral("-") : id, ok ? QStringLiteral("1") : QStringLiteral("0"));
  logUi(line);
  const bool isStopAck = (prettyType == QStringLiteral("logger.stop") || prettyType == QStringLiteral("session.stop"));
  if (isStopAck) {
    logUi(tr("[gui] Stop ACK received (id=%1, ok=%2)").arg(id.isEmpty() ? QStringLiteral("-") : id, ok ? QStringLiteral("1") : QStringLiteral("0")));
    updateLoggerControls();
    requestDeferredStatusRefresh();
  }
}

void MainWindow::onEkinoxPresenceChanged(bool online) {
  ekinoxPresenceOnline_ = online;
  logUi(tr("[ekinox] presence %1").arg(online ? tr("online") : tr("offline")));
  setPresenceLed(online);
  updateBackendHealth();
  updateLoggerControls();
}

void MainWindow::onEkinoxStatusChanged(const EkinoxStatus& st) {
  hasEkinoxStatus_ = true;
  lastEkinoxStatus_ = st;
  logUi(tr("[ekinox] status mode=%1 recording=%2 session='%3' link_alive=%4")
            .arg(st.mode.isEmpty() ? tr("-") : st.mode)
            .arg(st.recording_active ? tr("1") : tr("0"))
            .arg(st.session_name.isEmpty() ? tr("-") : st.session_name)
            .arg(st.link_alive ? tr("1") : tr("0")));
  updateLoggerControls();
}

void MainWindow::onEkinoxHeartbeatReceived() {
  lastEkinoxHeartbeatMs_ = QDateTime::currentMSecsSinceEpoch();
  updateBackendHealth();
}

void MainWindow::updateUiForState(MqttConnectionState state) {
  switch (state) {
    case MqttConnectionState::Disconnected:
      ui->labelConnState->setText("Disconnected");
      ui->btnConnect->setText("Connect");
      ui->btnConnect->setEnabled(true);
      break;
    case MqttConnectionState::Connecting:
      ui->labelConnState->setText("Connecting...");
      ui->btnConnect->setText("Disconnect");
      ui->btnConnect->setEnabled(true);
      break;
    case MqttConnectionState::Connected:
      ui->labelConnState->setText("Connected");
      ui->btnConnect->setText("Disconnect");
      ui->btnConnect->setEnabled(true);
      break;
    case MqttConnectionState::Stopping:
      ui->labelConnState->setText("Disconnecting...");
      ui->btnConnect->setText("Disconnect");
      ui->btnConnect->setEnabled(false);
      break;
  }
  updateLoggerControls();
}

void MainWindow::updateBackendHealth() {
  const bool mqttConnected = (clientState_ == MqttConnectionState::Connected);
  const bool ekinoxOnline = ekinoxPresenceOnline_;
  BackendHealth next = BackendHealth::Offline;
  if (mqttConnected && ekinoxOnline) {
    next = BackendHealth::Online;
  }
  setBackendLed(next);

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 ekinoxHbAge = (lastEkinoxHeartbeatMs_ > 0) ? (now - lastEkinoxHeartbeatMs_) : -1;
  if (mqttConnected != lastLedLogMqttConnected_ ||
      ekinoxOnline != lastLedLogEkinoxOnline_ ||
      next != lastLedLogState_) {
    logUi(tr("[ui-led] mqtt=%1 ekinoxOnline=%2 ekinoxHbAge=%3")
              .arg(mqttConnected ? QStringLiteral("1") : QStringLiteral("0"))
              .arg(ekinoxOnline ? QStringLiteral("1") : QStringLiteral("0"))
              .arg(ekinoxHbAge));
    lastLedLogMqttConnected_ = mqttConnected;
    lastLedLogEkinoxOnline_ = ekinoxOnline;
    lastLedLogState_ = next;
  }
}

void MainWindow::updateLoggerControls() {
  if (!ui) return;
  const bool connected = (clientState_ == MqttConnectionState::Connected);
  const bool ekinoxLinkAlive = ekinoxPresenceOnline_ || (hasEkinoxStatus_ && lastEkinoxStatus_.link_alive);
  const bool recording = hasEkinoxStatus_ && lastEkinoxStatus_.recording_active;
  const bool enableStart = connected && ekinoxLinkAlive && !recording;
  const bool enableStop = connected && ekinoxLinkAlive && recording;
  const bool enableSessionInput = !recording;

  const auto logStateChange = [this](const QString& control, bool enabled) {
    logUi(tr("[ui] %1 %2").arg(control, enabled ? tr("enabled") : tr("disabled")));
  };

  if (!loggerControlsInitialized_ || enableStart != lastStartEnabled_) {
    logStateChange(tr("Start session"), enableStart);
    lastStartEnabled_ = enableStart;
  }
  if (!loggerControlsInitialized_ || enableStop != lastStopEnabled_) {
    logStateChange(tr("Stop session"), enableStop);
    lastStopEnabled_ = enableStop;
  }

  ui->btnLoggerStart->setEnabled(enableStart);
  ui->btnLoggerStop->setEnabled(enableStop);
  ui->editSessionName->setEnabled(enableSessionInput);

  if (!loggerControlsInitialized_ || recording != lastRecordingActive_) {
    logUi(tr("[ui] Recording state -> %1").arg(recording ? tr("active") : tr("idle")));
    lastRecordingActive_ = recording;
  }

  if (!enableStop) {
    logUi(tr("[ui] Stop disabled because: mqttConnected=%1 ekinoxLinkAlive=%2 recordingActive=%3 hasEkinoxStatus=%4")
              .arg(connected ? "1" : "0")
              .arg(ekinoxLinkAlive ? "1" : "0")
              .arg(recording ? "1" : "0")
              .arg(hasEkinoxStatus_ ? "1" : "0"));
  }

  loggerControlsInitialized_ = true;

  const QString indicator = recording ? tr("RECORDING") : tr("IDLE");
  const QString indicatorStyle = recording
      ? QStringLiteral("color:#1B5A2B;font-weight:bold;")
      : QStringLiteral("color:#888888;font-weight:bold;");
  ui->labelRecordingValue->setText(indicator);
  ui->labelRecordingValue->setStyleSheet(indicatorStyle);

  const QString sessionText = (hasEkinoxStatus_ && !lastEkinoxStatus_.session_name.isEmpty())
      ? lastEkinoxStatus_.session_name
      : QStringLiteral("—");
  ui->labelSessionValue->setText(sessionText);
}

void MainWindow::onLoggerStartClicked() {
  if (clientState_ != MqttConnectionState::Connected) {
    logUi(tr("[gui] cannot start session while disconnected"));
    return;
  }

  const QString sessionName = desiredSessionName();
  const QString id = makeLoggerCmdId(QStringLiteral("gui_start_"));
  logUi(tr("[gui] Start clicked (session=%1, id=%2)").arg(sessionName, id));
  if (!mqtt_.publishLoggerStart(id, sessionName)) {
    logUi(tr("[gui] failed to publish logger.start"));
    return;
  }

  requestDeferredStatusRefresh();
}

void MainWindow::onLoggerStopClicked() {
  if (clientState_ != MqttConnectionState::Connected) {
    logUi(tr("[gui] cannot stop session while disconnected"));
    return;
  }

  const QString id = makeLoggerCmdId(QStringLiteral("gui_stop_"));
  logUi(tr("[gui] Stop clicked (id=%1)").arg(id));
  QJsonObject cmd;
  cmd.insert(QStringLiteral("type"), QStringLiteral("logger.stop"));
  cmd.insert(QStringLiteral("id"), id);
  const QString payload = QString::fromUtf8(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
  logUi(tr("[ui] publishing stop: %1").arg(payload));
  if (!mqtt_.publishLoggerStop(id)) {
    logUi(tr("[gui] failed to publish logger.stop"));
    return;
  }

  requestDeferredStatusRefresh();
}

QString MainWindow::desiredSessionName() const {
  if (!ui) return QStringLiteral("IronSoft_auto");
  const QString userValue = ui->editSessionName->text().trimmed();
  return userValue.isEmpty() ? generateSessionName() : userValue;
}

QString MainWindow::generateSessionName() const {
  return QStringLiteral("IronSoft_%1")
      .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
}

QString MainWindow::makeLoggerCmdId(const QString& prefix) {
  const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (prefix.isEmpty()) {
    return uuid;
  }
  return QStringLiteral("%1%2").arg(prefix, uuid);
}

void MainWindow::requestDeferredStatusRefresh() {
  QTimer::singleShot(1000, this, [this]() {
    if (clientState_ != MqttConnectionState::Connected) {
      logUi(tr("[gui] skipped logger.status (disconnected)"));
      return;
    }
    const QString id = makeLoggerCmdId(QStringLiteral("gui_status_"));
    if (!mqtt_.publishLoggerStatus(id)) {
      logUi(tr("[gui] failed to publish logger.status"));
    }
  });
}
