# Alif Zephyr SDK Setup for macOS

This README documents the complete setup and usage of the Alif Zephyr SDK on macOS, including building and flashing applications to Alif development boards.

## 📋 Prerequisites

- macOS (tested on macOS 15.0)
- Homebrew package manager
- Alif development board (E7, E3, E1C, or B1)
- USB cable for programming
- Alif Security Toolkit v1.98.3 or later

## 🛠️ Setup Instructions

### 1. Install Dependencies

```bash
# Install Homebrew dependencies
brew install python3 git wget cmake ninja

# Create Python virtual environment
python3 -m venv ~/zephyr-venv

# Activate virtual environment
source ~/zephyr-venv/bin/activate

# Install Python packages
python3 -m pip install west pyelftools
```

### 2. Download and Setup Zephyr SDK

```bash
# Download Zephyr SDK v0.16.5 for macOS
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.5/zephyr-sdk-0.16.5_macos-x86_64_minimal.tar.xz

# Extract the SDK
tar -xf zephyr-sdk-0.16.5_macos-x86_64_minimal.tar.xz

# Setup the ARM toolchain
cd zephyr-sdk-0.16.5
./setup.sh -t arm-zephyr-eabi -h -c
cd ..
```

### 3. Fetch Alif Zephyr SDK Source

```bash
# Create and initialize West workspace
mkdir sdk-alif
cd sdk-alif
west init -m https://github.com/alifsemi/sdk-alif.git --mr main

# Update all repositories (this may take several minutes)
west update
```

### 4. Download Alif Security Toolkit

- Download the Alif Security Toolkit from: https://alifsemi.com/download/APFW0002
- Extract to `~/code/app-release-exec-macos/`
- Ensure version is 1.98.3 or later

## 🚀 Building Applications

### Activate Environment

Always start by activating the Python virtual environment:

```bash
source ~/zephyr-venv/bin/activate
cd ~/code/sdk-alif/zephyr
```

### Build Hello World Application

#### For MRAM (Default - Recommended)
```bash
west build -b alif_e7_dk_rtss_he samples/hello_world
```

#### For ITCM
```bash
west build -b alif_e7_dk_rtss_he samples/hello_world \
  -DCONFIG_FLASH_BASE_ADDRESS=0 \
  -DCONFIG_FLASH_LOAD_OFFSET=0 \
  -DCONFIG_FLASH_SIZE=256
```

#### Other Supported Targets
- `alif_e7_dk_rtss_he` - E7 High Efficiency core
- `alif_e7_dk_rtss_hp` - E7 High Performance core
- `alif_e3_dk_rtss_he` - E3 High Efficiency core
- `alif_e3_dk_rtss_hp` - E3 High Performance core
- `alif_e1c_dk_rtss_he` - E1C High Efficiency core
- `alif_b1_dk_rtss_he` - B1 High Efficiency core

### Build Output

Successful builds produce:
- `build/zephyr/zephyr.bin` - Binary ready for flashing
- `build/zephyr/zephyr.elf` - ELF file with debug symbols
- Memory usage report showing flash and RAM consumption

## 📱 Flashing Applications

### 1. Prepare Application for Flashing

```bash
# Navigate to security toolkit directory
cd ~/code/app-release-exec-macos

# Copy the built binary
cp ~/code/sdk-alif/zephyr/build/zephyr/zephyr.bin build/images/zephyr_e7_rtsshe_helloworld.bin
```

### 2. Create Configuration File

Create `build/config/zephyr_e7_rtsshe_helloworld.json`:

```json
{
  "DEVICE": {
    "disabled": false,
    "binary": "app-device-config.json",
    "version": "0.5.00",
    "signed": true
  },
  "Zephyr-RTSS-HE": {
    "binary": "zephyr_e7_rtsshe_helloworld.bin",
    "version": "1.0.0",
    "cpu_id": "M55_HE",
    "mramAddress": "0x80000000",
    "flags": ["boot"],
    "signed": false
  }
}
```

For ITCM configuration, replace `"mramAddress": "0x80000000"` with:
```json
"loadAddress": "0x58000000",
"flags": ["load", "boot"]
```

### 3. Generate ATOC Image

```bash
./app-gen-toc -f build/config/zephyr_e7_rtsshe_helloworld.json
```

### 4. Flash to Device

```bash
# Connect your development board via USB
# Flash the application
./app-write-mram -p

# When prompted, select the USB port (typically /dev/cu.usbmodemXXXXXX)
```

### 5. Connect Serial Console

```bash
# Connect to serial console (replace with your actual port)
screen /dev/cu.usbmodemXXXXXX 115200

# Press reset button on development board
```

Expected output:
```
*** Booting Zephyr OS build zas-v1.2-178-g14a30a93eb0b ****
Hello World ! alif_e7_devkit
```

## 📁 Directory Structure

```
~/code/
├── sdk-alif/                    # Alif Zephyr SDK workspace
│   ├── zephyr/                  # Main Zephyr repository
│   ├── modules/                 # HAL and other modules
│   ├── bootloader/              # MCUboot bootloader
│   └── alif/                    # Alif-specific configurations
├── app-release-exec-macos/      # Alif Security Toolkit
│   ├── app-gen-toc*             # ATOC generation tool
│   ├── app-write-mram*          # Flash programming tool
│   └── build/                   # Build artifacts
├── zephyr-sdk-0.16.5/           # Zephyr SDK toolchain
└── ~/zephyr-venv/               # Python virtual environment
```

## 🔧 Troubleshooting

### Build Issues

1. **CMake not found**: Ensure cmake is installed via Homebrew
2. **Toolchain issues**: Verify Zephyr SDK setup completed successfully
3. **Python module errors**: Check virtual environment is activated

### Flashing Issues

1. **Port not found**: Check USB connection and try different ports
2. **Permission denied**: Run `sudo usermod -a -G dialout $USER` (may need to log out/in)
3. **Device not detected**: Press reset button on development board
4. **Flash size warnings**: Add `-p` flag to pad binary to required size

### Serial Console Issues

1. **No output**: Check baud rate (115200) and correct port
2. **Garbled text**: Verify UART configuration matches (8N1)
3. **Connection refused**: Close other terminal applications using the port

## 📚 Additional Resources

- [Alif Semiconductor Documentation](https://alifsemi.com/download/APFW0002)
- [Zephyr Project Documentation](https://docs.zephyrproject.org/)
- [West Tool Guide](https://docs.zephyrproject.org/latest/develop/west/index.html)

## 🏗️ Development Workflow

### Daily Development

```bash
# 1. Activate environment
source ~/zephyr-venv/bin/activate

# 2. Navigate to project
cd ~/code/sdk-alif/zephyr

# 3. Build your application
west build -b alif_e7_dk_rtss_he samples/your_app

# 4. Flash to device
cd ~/code/app-release-exec-macos
cp ~/code/sdk-alif/zephyr/build/zephyr/zephyr.bin build/images/your_app.bin
# Update JSON config file as needed
./app-gen-toc -f build/config/your_app.json
./app-write-mram -p
```

### Exploring Samples

The SDK includes many sample applications in `~/code/sdk-alif/zephyr/samples/`:

- `basic/blinky` - LED blinking example
- `drivers/gpio` - GPIO driver examples
- `net/` - Networking examples
- `bluetooth/` - Bluetooth LE examples
- `sensor/` - Sensor interface examples

### Memory Configurations

| Configuration | RTSS-HE Address | RTSS-HP Address | Use Case |
|---------------|-----------------|-----------------|----------|
| MRAM (default)| 0x80000000     | 0x80200000      | Production apps |
| ITCM          | 0x58000000     | 0x50000000      | Fast execution |

## 📝 Notes

- Always use the virtual environment when working with Zephyr
- The security toolkit version should be 1.98.3 or later
- MRAM configuration is recommended for most applications
- Press reset button after flashing to start the application
- Serial console uses 115200 baud, 8N1 configuration

## 🆘 Getting Help

If you encounter issues:

1. Check this README for troubleshooting steps
2. Verify all prerequisites are installed
3. Ensure development board is properly connected
4. Review the Alif Security Toolkit User Guide
5. Consult Zephyr documentation for framework-specific issues

---

*Setup completed and tested on macOS 15.0 with Alif E7 development kit.*