# NFC duty cycle simulation: nfc.c against a simulated Tonie (privacy mode, loses
# power when the field is off) on a virtual clock. make -C tests/host/nfc_sim
MAIN := ../../../teddybox/main

test: sim
	./sim
	./sim plain

# nfc.c is copied at every build (not kept in git) so that its includes
# resolve to the stubs, not to main/
sim: sim.c $(MAIN)/nfc.c $(MAIN)/nfc.h
	cp $(MAIN)/nfc.c nfc_copy.c
	cp $(MAIN)/nfc.h stub/nfc.h
	$(CC) -O1 -w -Istub -include stdbool.h sim.c nfc_copy.c -o $@

clean:
	rm -f sim nfc_copy.c stub/nfc.h
