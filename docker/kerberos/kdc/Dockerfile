FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        krb5-kdc \
        krb5-admin-server \
        krb5-user \
    && rm -rf /var/lib/apt/lists/*

COPY kdc.conf /etc/krb5kdc/kdc.conf
COPY kadm5.acl /etc/krb5kdc/kadm5.acl
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 88 88/udp 749

ENTRYPOINT ["/entrypoint.sh"]
