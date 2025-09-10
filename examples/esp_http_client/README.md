# ESP32 Post Example

## Setup Instructions

### Install Espressif IDF

```sh
# cd somewhere you wish to clone the ESP IDF to
cd <my_tools>

# Clone this specific version of the IDF
git clone -b v5.2.2 https://github.com/espressif/esp-idf.git

# Jump inside the IDF and setup your tools for this terminal session
cd esp-idf
source install.sh
source export.sh
```

### Jump over to this project

```sh
cd <my_projects>/esp_http_client
```

### Configure your build

```sh
# Set the chip you want to target
idf.py set-target esp32s3

# Configure the WiFi SSID & password
idf.py menuconfig

#
#   Example Connection Configuration -> WiFi SSID 
#                                       WiFi Password
```

### Build the project

```sh
idf.py build
```

### Flash and view the logs

```sh
idf.py flash monitor

# Once flashed, you should see the output:
# 
#   I (26) boot: ESP-IDF v5.2.2-dirty 2nd stage bootloader
#   I (27) boot: compile time Sep 10 2025 11:32:39
#   I (27) boot: Multicore bootloader
#   I (30) boot: chip revision: v0.1
#   I (34) boot.esp32s3: Boot SPI Speed : 80MHz
#   I (39) boot.esp32s3: SPI Mode       : DIO
#   I (44) boot.esp32s3: SPI Flash Size : 2MB
#   ...
#   I (5461) HTTP_CLIENT: Connected to AP
#   I (5471) main_task: Returned from app_main()
#   I (5481) wifi:<ba-add>idx:1 (ifx:0, 26:5a:4c:16:09:0b), tid:0, ssn:2, winSize:64
#   I (7441) HTTP_CLIENT: HTTPS Status = 400, content_length = 58
#   I (7441) HTTP_CLIENT: HTTP_EVENT_DISCONNECTED
```