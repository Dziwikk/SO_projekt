CC = gcc
CFLAGS = -pthread
TARGETS = sternik kasjer policjant pasazer orchestrator

all: $(TARGETS)

sternik: sternik.c
	$(CC) $(CFLAGS) -o $@ $<

kasjer: kasjer.c
	$(CC) $(CFLAGS) -o $@ $<

policjant: policjant.c
	$(CC) $(CFLAGS) -o $@ $<

pasazer: pasazer.c
	$(CC) $(CFLAGS) -o $@ $<

orchestrator: orchestrator.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
