.DEFAULT_GOAL := clean

.PHONY: loki mimir tempo demo clean

loki: # ./loki-demo.sh status to check what is up
	bash scripts/loki-demo.sh up

mimir: # ./mimir-demo.sh status to check what is up
	bash scripts/mimir-demo.sh up

tempo: # ./tempo-demo.sh status to check what is up
	bash scripts/tempo-demo.sh up

demo: # ./demo.sh status to check what is up
	bash scripts/demo.sh up

clean: # remove generated process logs
	rm -f *.log