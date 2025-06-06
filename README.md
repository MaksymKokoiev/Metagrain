# Metagrain - Granular Synthesizer Metasound Node for Unreal Engine

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) 

[Download Latest Release v0.5.0a](https://github.com/MaksymKokoiev/Metagrain/releases/download/v0.5.0a/Metagrain_0_5_0a.zip)

[Installation Instructions Below](#installation)

[Features](#features)

[Roadmap](#roadmap)

[Contribute](#contributing)

## Introduction

A custom Metasound node for Unreal Engine that provides powerful and flexible granular synthesis capabilities. 

This node allows sound designers and developers to create rich, evolving textures, soundscapes, and unique audio effects by deconstructing audio assets into small fragments (grains) and re-synthesizing them with a high degree of control.

Made by Maksym Kokoiev [Max Koko](https://maxkokomusic.com/)

in Collaboration with Wouter Meijer [Wouter Auris](https://auris-media.com/)

![Alt text](Metagrain_GranularSynth1.jpg?raw=true)


## Installation
### Requirements
Unreal Engine >5.4

This release was not tested with Unreal Engine versions previous to 5.4

### PC
1.  **Download:**
    * Download the latest realese
2.  **Add to Project:**
    * Create a `/Plugins` folder in your Unreal Engine project's root directory if it doesn't already exist.
    * Unzip the archive you downloaded inside `/Plugins`
    * The structure should be something like `MyProject/Plugins/Metagrain/..`
3.  **Enable Plugin:**
    * Open your project in Unreal Editor.
    * Go to `Edit > Plugins`.
    * Find `Metagrain` plugin and ensure it is enabled.
    * Restart the editor if prompted.

  ### Mac OS
  ⚠️ This release **does not include pre-compiled binaries for macOS.** 
  Mac users will need to compile the plugin from source by following the instructions below.

**Instructions to Compile on macOS:**

1. **Download**
   * Download the Source code (zip) from this release page.
2. **Add to your UE5 Project**
   * Navigate to your Unreal Engine project's root directory. Create a Plugins folder if it doesn't already exist.
   * Unzip the downloaded archive and place the Metagrain folder inside your Plugins directory.
3. **Build**
   * Right-click your project's .uproject file and select Services > Generate Xcode Project.
   * Open the generated .xcworkspace file in Xcode.
   * Select your project as the build target (e.g., "MyProjectEditor - Mac") and build it by pressing Cmd+B.
   * Once the build is successful, you can launch the editor and the plugin will be available to use.

## Features

Metagrain is a Plugin for Unreal Engine 5 that adds a "Granular Synth" node that offers a set of features for detailed control over the granulation process:

### Core Granulation Engine
* **Wave Asset Input:** Load any `.wav` file as the source for granulation.

* **Grain Duration:** Control the base length of each grain in milliseconds.

* **Duration Randomization:** Introduce variability to grain length.

* **Active Voices (Density):** Define the target number of overlapping grains, influencing the density of the sound.

* **Time Jitter:** Randomize the spawn interval of grains for less uniform textures.


### Playback & Triggering
* **Play/Stop Triggers:** Standard Metasound triggers to start and stop grain generation.

* **Start Point:** Specify the base time (in seconds) within the audio asset from which grains are read.

* **Start Point Randomization:** Add random positive offsets to the grain reading start point.

* **Warm Start:** (Optional) Immediately trigger a burst of grains up to the "Active Voices" count when playback starts, providing an instant full sound.


### Individual Grain Manipulation
* **Reverse Chance:** Set a percentage chance for individual grains to play in reverse.

* **Attack & Decay Envelope:**
    * **Attack Time (Percent):** Control the attack phase duration as a percentage of the grain's total duration.
    * **Decay Time (Percent):** Control the decay phase duration as a percentage of the grain's total duration.
    * **Attack Curve & Decay Curve:** Shape the attack and decay envelopes using exponential curves.

* **Pitch Shift:** Apply a base pitch shift to grains in semitones.

* **Pitch Randomization:** Introduce random pitch variations to individual grains.

* **Pan:** Control the base stereo pan position of grains.

* **Pan Randomization:** Add random stereo panning to individual grains.

* **Volume Randomization:** Apply random volume reduction to grains.


### Outputs & Monitoring
* **Stereo Audio Output:** Standard left and right audio channel outputs.
* **Event Triggers:**
    * `On Play`: Triggers when playback is initiated.
    * `On Finished`: Triggers when playback stops (via Stop trigger or if no audio is loaded on Play).
    * `On Grain`: Triggers every time a new grain is successfully spawned. This is crucial for synchronizing external events or visualizing grain activity.
* **Individual Grain Parameter Outputs (Tied to `On Grain`):**
    * `Grain Start Time (s)`: The actual start time of the spawned grain within the source audio.
    * `Grain Duration (s)`: The actual duration of the spawned grain.
    * `Grain Reversed (bool)`: True if the spawned grain is playing in reverse.
    * `Grain Volume (0-1)`: The actual volume scale of the spawned grain.
    * `Grain Pitch (semitones)`: The actual pitch shift of the spawned grain.
    * `Grain Pan (-1 to 1)`: The actual stereo pan position of the spawned grain.


## Usage

1.  Open or create a Metasound Source asset in the Content Browser.
2.  Right-click in the Metasound graph editor to bring up the node search menu.
3.  Search for "Granular Synth"
4.  Click to add the node to your graph.
5.  Load or connect a `Wave Asset` input (e.g., from an "Input" node of type Wave Asset).
6.  Connect the `Play` and `Stop` triggers to control playback.
7.  Adjust the various input parameters to shape your granular sound.
8.  Connect the `Out Left` and `Out Right` pins to an `Output` node.
9.  (Optional) Use the `On Grain` trigger and its associated parameter outputs (Grain Start Time, Duration, etc.) to drive other Metasound logic, debug visualizations, or external Blueprint events.

### Example Use Cases:

* Creating atmospheric pads and drones.
* Generating complex, evolving sound textures.
* Sci-fi effects from everyday sounds.
* Abstract percussive elements.
* Vocal manipulation and glitch effects.

## Roadmap

This section outlines potential future improvements and features. Feel free to contribute or suggest new ideas!

* Optimize Attack and Decay Controls.
* Multi-channel Wave Asset Support: Handle source files with more than stereo channels appropriately for grain selection.
* Multi-channel Output Support: Handle output up to 5.1 regardless of the initial .wav channel count (add an LFE X-over) 
* Performance Optimizations: Investigate areas for further CPU optimization, especially with high voice counts.
* Create separate Nodes for different use-cases: separate nodes with limited set of features optimized for a specific use-case.

## Contributing

Contributions are welcome! If you'd like to contribute, please:

1.  Fork the repository.
2.  Create a new branch for your feature or bug fix.
3.  Make your changes.
4.  Submit a pull request with a clear description of your changes.

<!-- Optional: Add a section for Known Issues if any -->

## Credits and Acknowledgements

This project was originally developed by:
* **Maksym Kokoiev:** [maxkokomusic.com](https://maxkokomusic.com/)
* **Wouter Meijer:** [auris-media.com](https://auris-media.com/)

Additional thanks and acknowledgement for inspiration and foundational concepts to:
* **Charles Matthews** and his **Metasound Branches Project**: [github.com/matthewscharles/metasound-branches](https://github.com/matthewscharles/metasound-branches)


## License

This project is licensed under the MIT License - see the [LICENSE.md](LICENSE.md)
