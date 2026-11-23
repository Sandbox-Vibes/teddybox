# TeddyBox plugin for teddyCloud

A page in teddyCloud's web interface for boxes running TeddyBox.

- **Boxes**: every box teddyCloud knows, which of them run TeddyBox and in
  which version (TeddyBox reports "TeddyBox vX.Y.Z" over RTNL), the last
  connection, a hint when a newer release is on GitHub, and the link to the
  box's setup page in the home network (`http://teddybox-xxxx.local/`; it
  answers only in setup mode).
- **Audio to tonie**: choose a box, a tonie, a title and audio files. They
  are uploaded to teddyCloud's library (`teddybox/<title>-<time>/`), encoded
  there into one TAF file (one chapter per file, in name order, teddyCloud's
  `/api/fileEncode`, needs ffmpeg as in the teddyCloud images) and set as the
  tonie's content (`lib://teddybox/<title>-<time>.taf`). The box downloads it
  the next time the tonie is placed or at the next check. A tonie whose file
  was uploaded on the TeddyBox setup page is protected and keeps that file.
- **Live events**: teddyCloud's event stream (`/api/sse`): tonie placed,
  ears, volume, playback.

German when the browser prefers German, otherwise English. Follows
teddyCloud's light or dark theme.

## Install

Copy the folder `teddybox` into teddyCloud's plugin folder (`plugins` in
teddyCloud's web directory, e.g. `data/www/plugins/teddybox/` in the Docker
volume) and reload the web interface. teddyCloud lists it under Tonieboxes.

## Limits

- With the login of teddyCloud's web interface switched on, the plugin uses
  the browser's session of teddyCloud; if the API refuses, it says so.
- The update hint needs internet access in the browser (GitHub API).
- teddyCloud does not say which box sent a live event.

Based on the teddyCloud plugin template (theme adapter) by b1u3n4z9u1.
