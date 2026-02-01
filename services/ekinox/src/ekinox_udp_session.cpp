#include "ironsoft/ekinox/ekinox_udp_session.h"

#include <limits>

namespace ironsoft::ekinox {

namespace {
inline std::int64_t now_ms() {
	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}
}

EkinoxUdpSession::EkinoxUdpSession() = default;

EkinoxUdpSession::~EkinoxUdpSession() {
	close();
}

void EkinoxUdpSession::reset_last_rx() {
	last_rx_ms_.store(now_ms(), std::memory_order_relaxed);
}

bool EkinoxUdpSession::is_connected() const noexcept {
	return connected_;
}

std::int64_t EkinoxUdpSession::last_rx_age_ms(std::chrono::steady_clock::time_point now) const noexcept {
	const auto last = last_rx_ms_.load(std::memory_order_relaxed);
	if (last == 0) {
		return std::numeric_limits<std::int64_t>::max();
	}
	const auto now_value = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	return now_value - last;
}

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)

std::string EkinoxUdpSession::error_to_string(SbgErrorCode code) {
	switch (code) {
	case SBG_NO_ERROR:
		return "SBG_NO_ERROR";
	case SBG_NOT_READY:
		return "SBG_NOT_READY";
	case SBG_INVALID_PARAMETER:
		return "SBG_INVALID_PARAMETER";
	case SBG_NULL_POINTER:
		return "SBG_NULL_POINTER";
	case SBG_TIME_OUT:
		return "SBG_TIME_OUT";
	case SBG_ERROR:
		return "SBG_ERROR";
	default:
		return std::to_string(static_cast<int>(code));
	}
}

SbgErrorCode EkinoxUdpSession::on_receive_log(
	SbgEComHandle*,
	SbgEComClass,
	SbgEComMsgId,
	const SbgEComLogUnion*,
	void* user_data) {
	if (auto* self = static_cast<EkinoxUdpSession*>(user_data); self != nullptr) {
		self->reset_last_rx();
	}
	return SBG_NO_ERROR;
}

bool EkinoxUdpSession::open(const Config& cfg, std::string& err) {
	close();
	config_ = cfg;
	last_rx_ms_.store(0, std::memory_order_relaxed);
	const sbgIpAddress remote_addr = sbgNetworkIpFromString(cfg.ip.c_str());

	if (remote_addr == 0u) {
		err = "invalid ip";
		return false;
	}
	SbgErrorCode status = sbgInterfaceUdpCreate(&udp_interface_, remote_addr, cfg.remote_port, cfg.local_port);
	if (status != SBG_NO_ERROR) {
		err = "sbgInterfaceUdpCreate: " + error_to_string(status);
		return false;
	}

	sbgInterfaceUdpSetConnectedMode(&udp_interface_, true);

	status = sbgEComInit(&ecom_handle_, &udp_interface_);
	if (status != SBG_NO_ERROR) {
		err = "sbgEComInit: " + error_to_string(status);
		sbgInterfaceDestroy(&udp_interface_);
		return false;
	}

	sbgEComSetReceiveLogCallback(&ecom_handle_, &EkinoxUdpSession::on_receive_log, this);

	reset_last_rx();
	connected_ = true;
	return true;
}

void EkinoxUdpSession::close() {
	if (!connected_) {
		return;
	}
	sbgEComClose(&ecom_handle_);
	sbgInterfaceDestroy(&udp_interface_);
	connected_ = false;
}

bool EkinoxUdpSession::poll(std::string& err) {
	if (!connected_) {
		err = "not connected";
		return false;
	}
	const SbgErrorCode status = sbgEComHandleOneLog(&ecom_handle_);
	if (status == SBG_NO_ERROR || status == SBG_NOT_READY) {
		return true;
	}
	if (status == SBG_TIME_OUT) {
		return true;
	}
	err = "sbgEComHandleOneLog: " + error_to_string(status);
	return false;
}

SbgEComHandle* EkinoxUdpSession::ecom_handle() noexcept {
	if (!connected_) {
		return nullptr;
	}
	return &ecom_handle_;
}

#else  // DEKINOX_HAS_SBG

bool EkinoxUdpSession::open(const Config&, std::string& err) {
	err = "sbgECom unavailable";
	return false;
}

void EkinoxUdpSession::close() {}

bool EkinoxUdpSession::poll(std::string& err) {
	err = "sbgECom unavailable";
	return false;
}

#endif  // DEKINOX_HAS_SBG

}  // namespace ironsoft::ekinox
