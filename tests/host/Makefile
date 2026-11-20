# Host side unit tests, no ESP-IDF needed:  make -C tests/host
MAIN := ../../teddybox/main
DAC := ../../teddybox/components/toniebox/dac3100
CFLAGS := -Wall -Wextra -O1 -I$(MAIN)

test: test_mqtt_ha_payload test_freshness test_settings test_speaker_eq test_ogg_pos test_backup_zip test_maint_time test_ogg_demux
	./test_backup_zip /tmp/teddybox_test_backup.zip
	./test_maint_time
	./test_ogg_demux
	python3 -c "import zipfile; z=zipfile.ZipFile('/tmp/teddybox_test_backup.zip'); assert z.testzip() is None; assert all(z.read(n)==open('/tmp/teddybox_test_backup.zip.'+n,'rb').read() for n in ('ca.der','client.der','private.der')); print('backup zip opens, files identical')"
	./test_ogg_pos
	./test_speaker_eq
	$(MAKE) -C nfc_sim
	$(MAKE) -C battery_sim
	$(MAKE) -C dac_sim
	./test_settings
	./test_freshness
	./test_mqtt_ha_payload
	./test_mqtt_ha_payload discovery | python3 -c "import sys,json; [json.loads(l.split('\t')[1]) for l in sys.stdin]; print('discovery JSON ok')"
	./test_mqtt_ha_payload state | python3 check_state.py

test_mqtt_ha_payload: test_mqtt_ha_payload.c $(MAIN)/mqtt_ha_payload.c
	$(CC) $(CFLAGS) $^ -o $@

# freshness.c with ESP-IDF headers replaced by small stubs
test_freshness: test_freshness.c $(MAIN)/freshness.c
	$(CC) -Wall -Wextra -O1 -Wno-unused-parameter -Istub -I$(MAIN) -I$(MAIN)/proto -I$(MAIN)/proto/proto $^ stub/freshness_stubs.c $(MAIN)/proto/protobuf-c.c $(MAIN)/proto/proto/toniebox.pb.taf-header.pb-c.c $(MAIN)/proto/proto/toniebox.pb.freshness-check.fc-request.pb-c.c -o $@

test_settings: test_settings.c $(MAIN)/settings.c $(DAC)/speaker_eq.c
	$(CC) -Wall -Wextra -O1 -Wno-unused-parameter -Istub -I$(MAIN) -I$(DAC) -DSETTINGS_INI_PATH='"/tmp/teddybox_test.ini"' $^ -lm -o $@

# certificate backup ZIP, checked with python's zipfile
test_ogg_demux: test_ogg_demux.c $(MAIN)/ogg_demux.c
	$(CC) $(CFLAGS) -o $@ $^

maint_time_only.c: $(MAIN)/maint.c gen_maint_time.sh
	./gen_maint_time.sh $< $@

test_maint_time: test_maint_time.c maint_time_only.c
	$(CC) $(CFLAGS) -include stdbool.h -o $@ $^

test_backup_zip: test_backup_zip.c $(MAIN)/backup_zip.c
	$(CC) $(CFLAGS) -Istub $^ -lz -o $@

test_ogg_pos: test_ogg_pos.c $(MAIN)/ogg_pos.c
	$(CC) $(CFLAGS) $^ -o $@

# EQ response computed from the quantized DAC coefficients
test_speaker_eq: test_speaker_eq.c $(DAC)/speaker_eq.c
	$(CC) $(CFLAGS) -I$(DAC) $^ -lm -o $@

clean:
	$(MAKE) -C nfc_sim clean
	$(MAKE) -C battery_sim clean
	$(MAKE) -C dac_sim clean
	rm -f test_mqtt_ha_payload test_freshness test_settings test_speaker_eq test_ogg_pos test_backup_zip test_maint_time test_ogg_demux
