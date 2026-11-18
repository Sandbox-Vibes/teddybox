# Host side unit tests, no ESP-IDF needed:  make -C tests/host
MAIN := ../../teddybox/main
CFLAGS := -Wall -Wextra -O1 -I$(MAIN)

test: test_mqtt_ha_payload
	./test_mqtt_ha_payload
	./test_mqtt_ha_payload discovery | python3 -c "import sys,json; [json.loads(l.split('\t')[1]) for l in sys.stdin]; print('discovery JSON ok')"

test_mqtt_ha_payload: test_mqtt_ha_payload.c $(MAIN)/mqtt_ha_payload.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f test_mqtt_ha_payload
