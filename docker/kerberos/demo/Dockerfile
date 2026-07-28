FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        krb5-user \
        python3 \
        python3-gssapi \
    && rm -rf /var/lib/apt/lists/*

COPY server.py client.py /app/

WORKDIR /app
