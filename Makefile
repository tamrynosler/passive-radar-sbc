JETSON  := jetson
REMOTE  := ~/pbr-cuda
RSYNC   := rsync -av --delete \
           --exclude '.git' --exclude 'build' --exclude 'results'

sync:
	$(RSYNC) ./ $(JETSON):$(REMOTE)/

build: sync
	ssh $(JETSON) 'cd $(REMOTE) && make -f jetson.mk'

test: build
	ssh $(JETSON) 'cd $(REMOTE) && make -f jetson.mk run'

# Run harness and append CSV to /data, then pull results back
bench: build
	ssh $(JETSON) 'cd $(REMOTE) && ./build/cufft_harness >> /data/results/cufft_timings.csv'
	mkdir -p results
	scp $(JETSON):/data/results/cufft_timings.csv results/

shell:
	ssh -t $(JETSON) 'cd $(REMOTE) && exec bash'

clean-remote:
	ssh $(JETSON) 'cd $(REMOTE) && make -f jetson.mk clean'

.PHONY: sync build test bench shell clean-remote
