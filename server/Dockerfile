# Gate Controller IDE + compile server (arduino-cli with the ESP32 core).
# Build from the repository root:  docker compose up -d --build
FROM python:3.12-slim

ARG ARDUINO_CLI_VERSION=1.6.0
ARG ESP32_CORE=3.3.12

RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://downloads.arduino.cc/arduino-cli/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
    | tar -xz -C /usr/local/bin arduino-cli

ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino \
    ARDUINO_DIRECTORIES_DOWNLOADS=/tmp/arduino-staging \
    ARDUINO_DIRECTORIES_USER=/opt/arduino-user \
    ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://espressif.github.io/arduino-esp32/package_esp32_index.json

# Install the ESP32 core, then drop what a classic-ESP32 build never uses
# (other chips' SDKs, the RISC-V compiler, debuggers) to keep the image small.
RUN arduino-cli core update-index \
 && arduino-cli core install "esp32:esp32@${ESP32_CORE}" \
 && cd /opt/arduino/packages/esp32/tools \
 && rm -rf esp32s2-libs esp32s3-libs esp32c2-libs esp32c3-libs esp32c5-libs esp32c6-libs esp32c61-libs \
           esp32h2-libs esp32p4-libs esp32p4_es-libs riscv32-esp-elf riscv32-esp-elf-gdb \
           xtensa-esp-elf-gdb openocd-esp32 \
 && rm -rf /tmp/arduino-staging \
 && du -sh /opt/arduino

WORKDIR /app
COPY server/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY server/app.py project.json ./
COPY firmware ./firmware
COPY web ./web

# Libraries listed in firmware/libraries.txt
RUN grep -vE '^\s*(#|$)' firmware/libraries.txt | while read -r lib; do arduino-cli lib install "$lib"; done

# Warm the compile cache so the first build from the IDE is fast
RUN mkdir -p /tmp/warm/firmware && cp firmware/*.ino firmware/*.h /tmp/warm/firmware/ \
 && arduino-cli compile --fqbn esp32:esp32:esp32 --build-path /tmp/gc-build/out /tmp/warm/firmware \
 && rm -rf /tmp/warm

VOLUME ["/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s CMD curl -fsS http://localhost:8080/healthz || exit 1
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080"]
