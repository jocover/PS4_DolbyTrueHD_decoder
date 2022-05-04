# PS4 hardware Dolby TrueHD decoder

Decode Dolby TrueHD to Linear PCM audio data

##### how to decode and play Dolby TrueHD audio on Playstation 4

```
ffmpeg -re -i filename -map 0:a:0 -c:a copy -bsf:a truehd_core -f truehd udp://PS4_IP:7721
```
