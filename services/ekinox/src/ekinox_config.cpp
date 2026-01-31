#include "ironsoft/ekinox/ekinox_config.h"

#include <fstream>
#include <memory>
#include <sstream>

namespace ironsoft::ekinox {

namespace {
constexpr int kMinPort = 0;
constexpr int kMaxPort = 65535;

bool validate_port(const Json::Value& value, const char* field, bool allow_zero, std::string& err, int& out) {
	if (!value.isInt()) {
		err = std::string{"invalid "} + field;
		return false;
	}
	const int port = value.asInt();
	const int min_port = allow_zero ? kMinPort : 1;
	if (port < min_port || port > kMaxPort) {
		err = std::string{"invalid "} + field;
		return false;
	}
	out = port;
	return true;
}

bool validate_positive_timeout(const Json::Value& value, const char* field, std::string& err, int& out) {
	if (!value.isInt()) {
		err = std::string{"invalid "} + field;
		return false;
	}
	const int val = value.asInt();
	if (val <= 0) {
		err = std::string{"invalid "} + field;
		return false;
	}
	out = val;
	return true;
}
}  // namespace

bool load_json_file(const std::string& path, Json::Value& out, std::string& err) {
	std::ifstream input(path);
	if (!input) {
		err = "failed to open " + path;
		return false;
	}

	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string content = buffer.str();

	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;

	std::string parse_errors;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(content.data(), content.data() + content.size(), &out, &parse_errors)) {
		err = parse_errors;
		return false;
	}

	err.clear();
	return true;
}

bool loadEkinoxConfig(const std::string& path, EkinoxConfig& cfg, std::string& err) {
	Json::Value root;
	if (!load_json_file(path, root, err)) {
		return false;
	}
	if (!root.isObject()) {
		err = "config root must be object";
		return false;
	}

	cfg = EkinoxConfig{};

	const Json::Value& mqtt = root["mqtt"];
	if (!mqtt.isObject()) {
		err = "missing mqtt";
		return false;
	}

	const Json::Value& mqtt_host = mqtt["host"];
	if (!mqtt_host.isString() || mqtt_host.asString().empty()) {
		err = "missing mqtt.host";
		return false;
	}
	cfg.mqtt.host = mqtt_host.asString();

	const Json::Value& mqtt_port = mqtt["port"];
	if (!validate_port(mqtt_port, "mqtt.port", false, err, cfg.mqtt.port)) {
		return false;
	}

	const Json::Value& mqtt_drone = mqtt["drone_id"];
	if (!mqtt_drone.isString() || mqtt_drone.asString().empty()) {
		err = "missing mqtt.drone_id";
		return false;
	}
	cfg.mqtt.drone_id = mqtt_drone.asString();

	if (const Json::Value& auth = mqtt["auth"]; auth.isObject()) {
		if (auth["username"].isString()) {
			cfg.mqtt.username = auth["username"].asString();
		}
		if (auth["password"].isString()) {
			cfg.mqtt.password = auth["password"].asString();
		}
	}

	if (const Json::Value& tls = mqtt["tls"]; tls.isObject()) {
		if (tls["enabled"].isBool()) {
			cfg.mqtt.tls_enabled = tls["enabled"].asBool();
		}
		if (tls["ca_file"].isString()) {
			cfg.mqtt.ca_file = tls["ca_file"].asString();
		}
	}

	const Json::Value& ekinox = root["ekinox"];
	if (!ekinox.isObject()) {
		err = "missing ekinox";
		return false;
	}

	const Json::Value& ekinox_ip = ekinox["ip"];
	if (!ekinox_ip.isString() || ekinox_ip.asString().empty()) {
		err = "missing ekinox.ip";
		return false;
	}
	cfg.ekinox.ip = ekinox_ip.asString();

	if (const Json::Value& udp = ekinox["udp_port"]; !udp.isNull()) {
		if (!validate_port(udp, "ekinox.udp_port", true, err, cfg.ekinox.udp_port)) {
			return false;
		}
	}

	if (const Json::Value& udp_local = ekinox["udp_local_port"]; !udp_local.isNull()) {
		if (!validate_port(udp_local, "ekinox.udp_local_port", true, err, cfg.ekinox.udp_local_port)) {
			return false;
		}
	}

	if (const Json::Value& rest = ekinox["rest_port"]; !rest.isNull()) {
		if (!validate_port(rest, "ekinox.rest_port", true, err, cfg.ekinox.rest_port)) {
			return false;
		}
	}

	if (const Json::Value& timeouts = root["timeouts"]; timeouts.isObject()) {
		auto read_timeout = [&](const char* field, int& target) -> bool {
			const Json::Value& value = timeouts[field];
			if (value.isNull()) {
				return true;
			}
			return validate_positive_timeout(value, field, err, target);
		};

		if (!read_timeout("rx_dead_ms", cfg.timeouts.rx_dead_ms)) {
			return false;
		}
		if (!read_timeout("start_timeout_ms", cfg.timeouts.start_timeout_ms)) {
			return false;
		}
		if (!read_timeout("stop_timeout_ms", cfg.timeouts.stop_timeout_ms)) {
			return false;
		}
		if (!read_timeout("reconnect_backoff_ms", cfg.timeouts.reconnect_backoff_ms)) {
			return false;
		}
		if (!read_timeout("reconnect_backoff_max_ms", cfg.timeouts.reconnect_backoff_max_ms)) {
			return false;
		}
	}

	return true;
}

}  // namespace ironsoft::ekinox
