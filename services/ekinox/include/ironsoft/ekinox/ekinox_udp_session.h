#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
#include "sbgECom.h"
#include "sbgErrorCodes.h"
#include <interfaces/sbgInterface.h>
#include <interfaces/sbgInterfaceUdp.h>
#include <network/sbgNetwork.h>
#endif

namespace ironsoft::ekinox {

class EkinoxUdpSession {
public:
	struct Config {
		std::string ip;
		std::uint16_t remote_port = 0;
		std::uint16_t local_port = 0;
		std::uint32_t rx_dead_ms = 0;
	};

	EkinoxUdpSession();
	~EkinoxUdpSession();

	bool open(const Config& cfg, std::string& err);
	void close();
	bool poll(std::string& err);
	bool is_connected() const noexcept;
	std::int64_t last_rx_age_ms(std::chrono::steady_clock::time_point now) const noexcept;

private:
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	static SbgErrorCode on_receive_log(
	    SbgEComHandle* handle,
	    SbgEComClass msgClass,
	    SbgEComMsgId msg,
	    const SbgEComLogUnion* log_data,
	    void* user_data);
	static std::string error_to_string(SbgErrorCode code);
#endif

	void reset_last_rx();

private:
	Config config_{};
	std::atomic<std::int64_t> last_rx_ms_{0};
	bool connected_ = false;
#if defined(DEKINOX_HAS_SBG) && (DEKINOX_HAS_SBG == 1)
	SbgInterface udp_interface_{};
	SbgEComHandle ecom_handle_{};
#endif
};

}  // namespace ironsoft::ekinox
