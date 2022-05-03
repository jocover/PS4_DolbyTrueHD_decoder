# PS4 hardware Dolby TrueHD decoder

Decode Dolby TrueHD to Linear PCM audio data

##### how to export truehd file

```
ffmpeg -i filename -map 0:a:0 -c:a copy -bsf:a truehd_core sample.thd
```
