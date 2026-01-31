#pragma once

#include <string_view>

namespace ironsoft::ekinox {

enum class EkinoxState {
	kUnknown = 0,
	kOffline,
	kInitializing,
	kReady,
	kRecording,
	kError
};

enum class EkinoxCommandType {
	kPing = 0,
	kStartRecording,
	kStopRecording
};

inline constexpr std::string_view kStateUnknownStr{"unknown"};
inline constexpr std::string_view kStateOfflineStr{"offline"};
inline constexpr std::string_view kStateInitializingStr{"initializing"};
inline constexpr std::string_view kStateReadyStr{"ready"};
inline constexpr std::string_view kStateRecordingStr{"recording"};
inline constexpr std::string_view kStateErrorStr{"error"};

inline constexpr std::string_view kCmdPingStr{"ping"};
inline constexpr std::string_view kCmdStartRecordingStr{"start_recording"};
inline constexpr std::string_view kCmdStopRecordingStr{"stop_recording"};

inline constexpr std::string_view to_string(EkinoxState state) noexcept {
	switch (state) {
	case EkinoxState::kOffline:
		return kStateOfflineStr;
	case EkinoxState::kInitializing:
		return kStateInitializingStr;
	case EkinoxState::kReady:
		return kStateReadyStr;
	case EkinoxState::kRecording:
		return kStateRecordingStr;
	case EkinoxState::kError:
		return kStateErrorStr;
	case EkinoxState::kUnknown:
	default:
		return kStateUnknownStr;
	}
}

inline constexpr bool try_parse_state(std::string_view text, EkinoxState& state) noexcept {
	if (text == kStateOfflineStr) {
		state = EkinoxState::kOffline;
		return true;
	}
	if (text == kStateInitializingStr) {
		state = EkinoxState::kInitializing;
		return true;
	}
	if (text == kStateReadyStr) {
		state = EkinoxState::kReady;
		return true;
	}
	if (text == kStateRecordingStr) {
		state = EkinoxState::kRecording;
		return true;
	}
	if (text == kStateErrorStr) {
		state = EkinoxState::kError;
		return true;
	}
	if (text == kStateUnknownStr) {
		state = EkinoxState::kUnknown;
		return true;
	}
	state = EkinoxState::kUnknown;
	return false;
}

inline constexpr std::string_view to_string(EkinoxCommandType type) noexcept {
	switch (type) {
	case EkinoxCommandType::kPing:
		return kCmdPingStr;
	case EkinoxCommandType::kStartRecording:
		return kCmdStartRecordingStr;
	case EkinoxCommandType::kStopRecording:
		return kCmdStopRecordingStr;
	default:
		return kCmdPingStr;
	}
}

inline constexpr bool try_parse_command_type(std::string_view text, EkinoxCommandType& type) noexcept {
	if (text == kCmdPingStr) {
		type = EkinoxCommandType::kPing;
		return true;
	}
	if (text == kCmdStartRecordingStr) {
		type = EkinoxCommandType::kStartRecording;
		return true;
	}
	if (text == kCmdStopRecordingStr) {
		type = EkinoxCommandType::kStopRecording;
		return true;
	}
	type = EkinoxCommandType::kPing;
	return false;
}

}  // namespace ironsoft::ekinox
