# Third-party component

This build embeds the background server from:
- cy33hc/ps4-ezremote-server
- pinned commit: 7967905c5d7928ce1888f9c6c34cd6510d97ad77
- license: GPL-3.0

The GitHub Actions workflow fetches and builds that source at build time. The browser application talks to the server only through its local HTTP API on 127.0.0.1:6701 and loads it through the GoldHEN BinLoader on 127.0.0.1:9090.

Upstream source: https://github.com/cy33hc/ps4-ezremote-server
