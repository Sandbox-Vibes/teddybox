#pragma once
/* Fetches the names of the tonies from teddyCloud's web API
   (GET http://<cloud_host>:<tc_web_port>/api/getTagInfo?ruid=...,
   series and episode from its tonies.json) into tb_names.txt: the tonie
   that is playing first, the others on the SD card one by one. Called from
   the cloud task while teddyCloud is the backend in use. */
void tonie_names_tc_step(void);
