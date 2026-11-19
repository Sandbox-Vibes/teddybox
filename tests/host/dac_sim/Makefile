# dac3100.c against a register model, checked against the original firmware's values
DAC := ../../../teddybox/components/toniebox/dac3100

test: test_dac
	./test_dac

test_dac: test_dac.c $(DAC)/dac3100.c $(DAC)/dac3100.h
	$(CC) -O1 -Wall -Wno-unused-function -Wno-unused-variable -Istub -I$(DAC) -include stubs.h test_dac.c $(DAC)/dac3100.c -o $@

clean:
	rm -f test_dac
