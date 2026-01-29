# RPi setup notes

## If `libpaho-mqttpp3-dev` is missing
You can build Paho MQTT C and C++ from source.

(Example steps – adjust versions as needed)

```bash
sudo apt update
sudo apt install -y git cmake g++ ninja-build libssl-dev

# Paho MQTT C
git clone https://github.com/eclipse/paho.mqtt.c.git
cd paho.mqtt.c
cmake -S . -B build -G Ninja -DPAHO_WITH_SSL=ON -DPAHO_BUILD_STATIC=OFF -DPAHO_BUILD_SHARED=ON
cmake --build build
sudo cmake --install build
cd ..

# Paho MQTT C++
git clone https://github.com/eclipse/paho.mqtt.cpp.git
cd paho.mqtt.cpp
cmake -S . -B build -G Ninja -DPAHO_MQTT_C_PATH=/usr/local -DPAHO_BUILD_STATIC=OFF -DPAHO_BUILD_SHARED=ON
cmake --build build
sudo cmake --install build
sudo ldconfig
```

Then build `backend/` normally.
