MCP_DIR := mcp-server

.PHONY: build-mcp test-mcp clean

build-mcp:
	cd $(MCP_DIR) && go build -o twitter-mcp-server .

test-mcp:
	cd $(MCP_DIR) && go test -v ./...

clean:
	rm -fr frontend backend database test utils observability UI_UX.md *.svg
