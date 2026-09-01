# WebRTC AEC Component with YARP Integration

This directory contains a YARP-based Acoustic Echo Cancellation (AEC) component that integrates the WebRTC audio processing library.

## Overview

The AEC component:
- Receives audio from two YARP ports:
  - **Reference input** (`/aecComponent/reference:i`): Speaker output (what is being played)
  - **Microphone input** (`/aecComponent/microphone:i`): Microphone recording (with echo)
- Processes both signals through WebRTC's AEC module
- Outputs echo-canceled audio (`/aecComponent/audio:o`)

## Architecture

```
Speaker Output ──┐
                 ├─→ [ProcessReverseStream] ──┐
                 │                            ├─→ [AEC Processing] ──→ Output
Microphone ──────┤                            │   (Echo Cancellation)   (Echo-canceled)
(with echo)      └─→ [ProcessStream] ─────────┘
```

## Components

### Files

- **AECComponent.hpp**: Header file defining the component interface
- **AECComponent.cpp**: Implementation of the AEC component
- **aec_main.cpp**: Main entry point
- **aec_config.ini**: Configuration file with default settings
- **CMakeLists.txt**: Build configuration

## Building

### Prerequisites

- YARP installed and configured
- WebRTC Audio Processing library installed separately
- CMake 3.10+
- C++17 compatible compiler

### Build Steps

```bash
cd /path/to/yarp-examples
mkdir build
cd build
export WEBRTC_AUDIO_PROCESSING_ROOT=/path/to/webrtc-audio-processing/install
cmake ..
make
```

If you prefer not to export the variable, you can still pass it with `-DWEBRTC_AUDIO_PROCESSING_ROOT=...`.

## Running the Component

### 1. Start YARP nameserver (if not already running)

```bash
yarpserver
```

### 2. Run the AEC component

```bash
./aecComponent
```

Or with custom config:

```bash
./aecComponent --from aec_config.ini
```

### 3. Connect YARP ports

Open another terminal and establish connections:

```bash
# Connect your robot's speaker output to the reference port
yarp connect /robot/audio_player /aecComponent/reference:i

# Connect your microphone to the microphone input
yarp connect /microphone/audio /aecComponent/microphone:i

# Connect the output to your processing pipeline
yarp connect /aecComponent/audio:o /your_component/audio:i
```

## Configuration

Edit `aec_config.ini` to customize:

- **referenceInputPort**: Port name for speaker reference signal
- **microphoneInputPort**: Port name for microphone input
- **audioOutputPort**: Port name for processed output
- **sample_rate**: Audio sample rate (Hz) - default 48000
- **num_channels**: Number of channels - default 1 (mono)
- **block_ms**: Processing block size in milliseconds - default 10
- **saveIterativeAudioToDisk**: Concatenate consecutive filtered, original microphone, and reference chunks into duration-limited WAV files - default false
- **audioSaveMaxSeconds**: Maximum duration per saved WAV file - default 30
- **audioSaveDirectory**: Directory where WAV files are written - default `./aec-recordings`
- **audioSavePrefix**: Prefix used for generated WAV filenames - default `aec_output`

Recorded files are named `<prefix>_filtered_NNNNNN.wav`,
`<prefix>_microphone_NNNNNN.wav`, and `<prefix>_reference_NNNNNN.wav`.
- **aec_mobile_mode**: Enables WebRTC mobile AEC mode (can improve aggressive echo suppression) - default false
- **aec_stream_delay_ms**: Estimated render-to-capture delay used by AEC alignment - default 120
- **logAecStats**: Print 1 Hz AEC activity stats (with/without reference frames) - default true

## Example Integration with Your Robot

```bash
# Terminal 1: Start YARP nameserver
yarpserver

# Terminal 2: Start the AEC component
cd /path/to/yarp-examples/build
./aecComponent

# Terminal 3: Connect ports (adjust names to match your robot)
yarp connect /robot/speaker:o /aecComponent/reference:i
yarp connect /robot/microphone:o /aecComponent/microphone:i
yarp connect /aecComponent/audio:o /robot/speech_recognizer:i
```

## Features

### Enabled Processing

The component enables the following WebRTC Audio Processing modules:

1. **Echo Cancellation (AEC)**
   - Reduces speaker echo from microphone signal
   - Non-mobile mode for better cancellation

2. **Gain Control 1 (Analog)**
   - Adaptive gain adjustment
   - Maintains consistent output level

3. **Gain Control 2**
   - Additional digital gain control

4. **High-Pass Filter**
   - Removes low-frequency noise

## Troubleshooting

### Audio Port Connection Issues

If connections fail, verify port availability:

```bash
yarp name list
```

### Audio Synchronization

The component processes synchronized stereo input (reference + microphone). Ensure:
- Both input ports are connected
- Audio streams arrive simultaneously
- Same sample rate and block size

### Performance

If processing drops frames:
- Increase the processing thread priority
- Reduce other system load
- Check CPU usage: `top` or `htop`

## Comparison with run-realtime.cpp

| Feature | run-realtime.cpp | AECComponent |
|---------|------------------|--------------|
| Audio Backend | PortAudio | YARP |
| Integration | Standalone | Robot middleware |
| Port-based I/O | No | Yes |
| Configuration | Compile-time | Runtime (INI file) |
| Multi-process | Single process | Distributed |
| Use Case | Testing/Development | Production integration |

## References

- [YARP Documentation](https://www.yarp.it/)
- [WebRTC Audio Processing](https://webrtc.googlesource.com/src)
