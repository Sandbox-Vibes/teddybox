# Battery rest-voltage estimate (load sag while playing): battery.c against a
# simulated voltage. battery.c is copied so that its includes resolve to the stubs.
MAIN := ../../../teddybox/main

test: sim
	./sim

sim: sim.c $(MAIN)/battery.c $(MAIN)/battery.h
	cp $(MAIN)/battery.c battery_copy.c
	cp $(MAIN)/battery.h stub/battery.h
	$(CC) -w -Istub -include stdbool.h sim.c -o $@

clean:
	rm -f sim battery_copy.c stub/battery.h
