FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ libcurl4-openssl-dev curl ca-certificates \
    iproute2 iputils-ping iptables arptables tcpdump strace libc-bin \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /lab
COPY curl_probe.cpp /lab/curl_probe.cpp
RUN g++ -std=c++17 -Wall -Wextra -Werror -pthread /lab/curl_probe.cpp \
    -lcurl -o /usr/local/bin/curl-probe
COPY resolver_trace.c /lab/resolver_trace.c
RUN gcc -std=c11 -Wall -Wextra -Werror -fPIC -shared -pthread \
    /lab/resolver_trace.c -ldl -o /usr/local/lib/libresolver_trace.so
CMD ["sleep", "infinity"]