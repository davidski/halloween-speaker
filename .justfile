# Build the firmware
build:
    pio run -e feather

# Build and push over the air (password from 1Password)
ota ip="192.168.20.116":
    pio run -e ota -t upload --upload-port {{ip}}
