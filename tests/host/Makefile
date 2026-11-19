# Host side unit tests, no ESP-IDF needed:  make -C tests/host
MAIN := ../../teddybox/main
DAC := ../../teddybox/components/toniebox/dac3100
CFLAGS := -Wall -Wextra -O1 -I$(MAIN)

test: test_mqtt_ha_payload test_freshness test_settings test_speaker_eq
	./test_speaker_eq
	$(MAKE) -C nfc_sim
	$(MAKE) -C dac_sim
	./test_settings
	./test_freshness
	./test_mqtt_ha_payload
	./test_mqtt_ha_payload discovery | python3 -c "import sys,json; [json.loads(l.split('\t')[1]) for l in sys.stdin]; print('discovery JSON ok')"

test_mqtt_ha_payload: test_mqtt_ha_payload.c $(MAIN)/mqtt_ha_payload.c
	$(CC) $(CFLAGS) $^ -o $@

# freshness.c with ESP-IDF headers replaced by small stubs
test_freshness: test_freshness.c $(MAIN)/freshness.c
	$(CC) -Wall -Wextra -O1 -Wno-unused-parameter -Istub -I$(MAIN) -I$(MAIN)/proto -I$(MAIN)/proto/proto $^ stub/freshness_stubs.c $(MAIN)/proto/protobuf-c.c $(MAIN)/proto/proto/toniebox.pb.taf-header.pb-c.c $(MAIN)/proto/proto/toniebox.pb.freshness-check.fc-request.pb-c.c -o $@

test_settings: test_settings.c $(MAIN)/settings.c $(DAC)/speaker_eq.c
	$(CC) -Wall -Wextra -O1 -Wno-unused-parameter -Istub -I$(MAIN) -I$(DAC) -DSETTINGS_INI_PATH='"/tmp/teddybox_test.ini"' $^ -lm -o $@

# EQ response computed from the quantized DAC coefficients
test_speaker_eq: test_speaker_eq.c $(DAC)/speaker_eq.c
	$(CC) $(CFLAGS) -I$(DAC) $^ -lm -o $@

clean:
	$(MAKE) -C nfc_sim clean
	$(MAKE) -C dac_sim clean
	rm -f test_mqtt_ha_payload test_freshness test_settings test_speaker_eq
