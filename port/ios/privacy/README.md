# Privacy manifests

The app bundle carries one `PrivacyInfo.xcprivacy`, chosen by the build:

- `plain/`: every ordinary build. The app sends nothing to anyone, so it declares no collected data.
- `tester/`: builds made with `tools/ios_build.py --tester`, which have the device benchmark's **Benchmark** button beside **Play**. Finishing the benchmark automatically queues and uploads a report when upload is enabled: frame times, memory and thermal state per run (performance data) and the device's model identifier, system version, GPU and display mode (other diagnostic data). It is not linked to the person and is not used for tracking.

Both declare the same required-reason APIs, which the game and its libraries use in every build: file timestamps of files inside the app's own container (`C617.1`), elapsed time measured inside the app (`35F9.1`), free disk space checked before writing (`E174.1`) and the app's own user defaults (`CA92.1`).
