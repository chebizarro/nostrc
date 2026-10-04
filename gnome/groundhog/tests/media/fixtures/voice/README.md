# Voice interop audio vectors

These are **real decodable audio**, not invented container headers. Generated with
FFmpeg from a 1.2-second sine tone, with metadata removed:

```sh
ffmpeg -f lavfi -i 'sine=frequency=440:duration=1.2:sample_rate=16000' -ac 1 -ar 16000 -c:a aac -profile:a aac_low -b:a 64000 -map_metadata -1 -movflags +faststart white-noise-aac.m4a
ffmpeg -f lavfi -i 'sine=frequency=660:duration=1.2:sample_rate=48000' -ac 1 -ar 48000 -c:a libopus -b:a 32000 -map_metadata -1 groundhog-opus.ogg
```

The AAC-LC/MP4 vector matches White Noise Android's `VoiceRecorder` format
(`audio/mp4`, `.m4a`, mono 16 kHz, 64 kb/s); it is not a byte-for-byte Android
recording. The Ogg/Opus vector matches Groundhog's `audio/ogg` send format.
White Noise uses Android `MediaPlayer` on the downloaded file, and Android's
supported-media table includes Ogg/Opus decoding. MDK 0.11 imeta has no
waveform or duration; Groundhog derives the waveform from decoded PCM after
explicit download and sends no unverified extra imeta fields.

Sources (pinned to the reviewed White Noise commit):
- https://github.com/marmot-protocol/whitenoise-android/blob/73aaf05039f4a2d957800f3f06eeac2eab374d55/app/src/main/java/dev/ipf/whitenoise/android/audio/VoiceRecorder.kt
- https://github.com/marmot-protocol/whitenoise-android/blob/73aaf05039f4a2d957800f3f06eeac2eab374d55/app/src/main/java/dev/ipf/whitenoise/android/audio/VoicePlaybackController.kt
- https://github.com/marmot-protocol/whitenoise-android/blob/73aaf05039f4a2d957800f3f06eeac2eab374d55/app/src/main/java/dev/ipf/whitenoise/android/ui/conversation/media/MediaVoice.kt
- https://developer.android.com/media/platform/supported-formats
