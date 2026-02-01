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
  }
  updateBackendHealth();
  updateLoggerControls();
}

void MainWindow::onPresenceChanged(bool online) {
  presenceOnline_ = online;
  if (!online) {
    lastHeartbeatMs_ = 0;
  }
  setPresenceLed(online);
  updateBackendHealth();
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
}

void MainWindow::onPingUpdated(const QString& /*id*/, qint64 rttMs, bool timeout) {
  setPingRttDisplay(rttMs, timeout);
}

void MainWindow::onAckReceived(const QString& type, const QString& id, bool ok) {
  const QString prettyType = type.isEmpty() ? QStringLiteral("-") : type;
  const QString line = tr("[ack] type=%1 id=%2 ok=%3").arg(prettyType, id.isEmpty() ? QStringLiteral("-") : id, ok ? QStringLiteral("1") : QStringLiteral("0"));
  logUi(line);
  if (prettyType == QStringLiteral("logger.stop") || prettyType == QStringLiteral("session.stop")) {
    updateLoggerControls();
    requestDeferredStatusRefresh();
  }
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
  BackendHealth next = BackendHealth::Offline;
  if (clientState_ == MqttConnectionState::Connected && presenceOnline_) {
    if (lastHeartbeatMs_ == 0) {
      next = BackendHealth::Stale;
    } else {
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      const qint64 delta = now - lastHeartbeatMs_;
      next = (delta <= kHeartbeatTimeoutMs) ? BackendHealth::Online : BackendHealth::Stale;
    }
  }
  setBackendLed(next);
}

void MainWindow::updateLoggerControls() {
  if (!ui) return;
  const bool connected = (clientState_ == MqttConnectionState::Connected);
  const bool recording = hasStatus_ && lastStatus_.recording_active;
  ui->btnLoggerStart->setEnabled(connected && !recording);
  ui->btnLoggerStop->setEnabled(connected && recording);

  const QString indicator = recording ? tr("RECORDING") : tr("IDLE");
  const QString indicatorStyle = recording
      ? QStringLiteral("color:#1B5A2B;font-weight:bold;")
      : QStringLiteral("color:#888888;font-weight:bold;");
  ui->labelRecordingValue->setText(indicator);
  ui->labelRecordingValue->setStyleSheet(indicatorStyle);

  const QString sessionText = (hasStatus_ && !lastStatus_.session_name.isEmpty())
      ? lastStatus_.session_name
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
  ++loggerCmdSeq_;
  return QStringLiteral("%1%2").arg(prefix).arg(loggerCmdSeq_);
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
