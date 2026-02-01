#pragma once

#include <string>
#include <string_view>

namespace ironsoft::ekinox {

enum class ServiceState {
	kDisconnected = 0,
	kConnecting,
	kIdle,
	kStarting,
	kRecording,
	kStopping,
	kError
};

enum class CommandType {
	kPing = 0,
	kLoggerStart,
	kLoggerStop,
	kLoggerStatus,
	kUnknown
};

inline constexpr std::string_view kStateDisconnected{"DISCONNECTED"};
inline constexpr std::string_view kStateConnecting{"CONNECTING"};
inline constexpr std::string_view kStateIdle{"IDLE"};
inline constexpr std::string_view kStateStarting{"STARTING"};
inline constexpr std::string_view kStateRecording{"RECORDING"};
inline constexpr std::string_view kStateStopping{"STOPPING"};
inline constexpr std::string_view kStateError{"ERROR"};

inline constexpr std::string_view kCmdPing{"ping"};
inline constexpr std::string_view kCmdLoggerStart{"logger.start"};
inline constexpr std::string_view kCmdLoggerStop{"logger.stop"};
inline constexpr std::string_view kCmdLoggerStatus{"logger.status"};

inline constexpr std::string_view to_string(ServiceState state) noexcept {
	switch (state) {
	case ServiceState::kConnecting:
		return kStateConnecting;
	case ServiceState::kIdle:
		return kStateIdle;
	case ServiceState::kStarting:
		return kStateStarting;
	case ServiceState::kRecording:
		return kStateRecording;
	case ServiceState::kStopping:
		return kStateStopping;
	case ServiceState::kError:
		return kStateError;
	case ServiceState::kDisconnected:
	default:
		return kStateDisconnected;
	}
}

inline constexpr bool try_parse_state(std::string_view text, ServiceState& state) noexcept {
	if (text == kStateDisconnected) {
		state = ServiceState::kDisconnected;
		return true;
	}
	if (text == kStateConnecting) {
		state = ServiceState::kConnecting;
		return true;
	}
	if (text == kStateIdle) {
		state = ServiceState::kIdle;
		return true;
	}
	if (text == kStateStarting) {
		state = ServiceState::kStarting;
		return true;
	}
	if (text == kStateRecording) {
		state = ServiceState::kRecording;
		return true;
	}
	if (text == kStateStopping) {
		state = ServiceState::kStopping;
		return true;
	}
	if (text == kStateError) {
		state = ServiceState::kError;
		return true;
	}
	state = ServiceState::kDisconnected;
	return false;
}

inline constexpr std::string_view to_string(CommandType type) noexcept {
	switch (type) {
	case CommandType::kPing:
		return kCmdPing;
	case CommandType::kLoggerStart:
		return kCmdLoggerStart;
	case CommandType::kLoggerStop:
		return kCmdLoggerStop;
	case CommandType::kLoggerStatus:
		return kCmdLoggerStatus;
	default:
		return kCmdPing;
	}
}

inline constexpr bool try_parse_command_type(std::string_view text, CommandType& type) noexcept {
	if (text == kCmdPing) {
		type = CommandType::kPing;
		return true;
	}
	if (text == kCmdLoggerStart) {
		type = CommandType::kLoggerStart;
		return true;
	}
	if (text == kCmdLoggerStop) {
		type = CommandType::kLoggerStop;
		return true;
	}
	if (text == kCmdLoggerStatus) {
		type = CommandType::kLoggerStatus;
		return true;
	}
	type = CommandType::kUnknown;
	return false;
}

}  // namespace ironsoft::ekinox
