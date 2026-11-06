# Standalone AAMP-Style Curl Probe

Separate C++17 application, not AAMP code. One download worker executes
curl_easy_perform(). The caller optionally requests cancellation and joins it.
No AAMP runtime, MPD parsing, WPE watchdog, DNS server or HTTP server is required.

## Thread Flow

```text
Caller thread                    Download worker
Start worker ------------------> Configure curl easy handle
Optional stop delay              curl_easy_perform(URL)
Clear active flag -------------> Progress callback requests abort
join(worker)                     Curl finishes, including resolver cleanup
                                 Log timings, clean up curl handle, exit
Join returns <------------------ Worker completes
```

The callback cannot forcibly interrupt getaddrinfo(). Join intentionally has
no timeout so this wait is observable. The probe cleans up its handle in the
worker; AAMP performs some cleanup after joining its MPD worker. This models
the relevant curl/thread pattern, not the complete AAMP lifecycle.

## Curl Configuration

| Setting | Value |
| --- | --- |
| TIMEOUT / CONNECTTIMEOUT | 20 / 10 seconds, logged incident manifest settings |
| NOSIGNAL / IPRESOLVE | 1 / WHATEVER |
| FOLLOWLOCATION / ACCEPT_ENCODING | 1 / empty string (supported encodings) |
| DNS_CACHE_TIMEOUT | 180 seconds, AAMP source default; verify device override |
| NOPROGRESS / XFERINFO | 0 / cancellation callback enabled |
| Start / stall / low-bandwidth timeout | 0 / 10 seconds / disabled |
| SSLVERSION | TLS 1.2 minimum |
| TLS verification | Disabled to match incident; --verify-peer enables it |
| Retries | 1 for relevant curl timeout/connect/send and HTTP 408/5xx; 10 for 502 |
| HTTP retry delay | 50 ms; 1,000 ms for 502 |

Options are based on the adjacent AAMP checkout's manifest setup. Generic
source timeout defaults are 5/3 seconds; 20/10 are the logged runtime overrides.
URL is required; user agent is a placeholder. Supply --user-agent, --header
and --proxy as needed. Empty proxy disables environment proxies explicitly.
Never send production credentials with TLS verification disabled.

Callbacks discard received content and model cancellation/start/stall checks
with an atomic flag. No two-downloader refresh/notifier machinery runs.
Retry/success mapping is simplified; probe success accepts HTTP 2xx.
Each invocation has a fresh curl DNS/connection cache; the OS neighbor cache
is independent. Ubuntu libcurl is not the exact device vendor build: compare
version, patches, architecture and resolver backend before claiming equivalence.

## Start Once (Host Terminal)

From the workspace root:

```sh
cd dns-curl-lab
docker compose up -d --build --remove-orphans
docker compose exec client sh
```

Compose starts only the client, with normal Docker networking and no published
ports or automatic faults. --remove-orphans removes the previous lab gateway
if present. Commands below run inside this Client shell, from any directory.
The executable is /usr/local/bin/curl-probe. Host results/ is mounted at /results.

## Healthy Baseline (Client Shell)

```sh
curl -V
cat /etc/resolv.conf
cat /etc/nsswitch.conf
ip route
ip neigh show
curl-probe --help
```

Choose a team-approved reachable URL. Replace this example with your endpoint:

```sh
curl-probe --url https://example.com/ --verify-peer
echo "probe_exit=$?"
curl-probe --url https://example.com/ --verify-peer --stop-after-ms 1000
echo "probe_exit=$?"
```

Success: curl_code=0, HTTP 200, worker_exit, join_end and probe_exit=0.
A fast request logs completed_before_release; it finished before stop was due.
Exit 1 means curl/HTTP failure or cancellation; 2 means setup failure.
The runtime line identifies the library actually used by this application.
AsynchDNS=yes does not prove c-ares; c_ares=none can mean threaded getaddrinfo.

## Modify resolv.conf (Client Shell, Manual Only)

Save the original configuration before changing it:

```sh
test -f /tmp/resolv.conf.before-test || cp /etc/resolv.conf /tmp/resolv.conf.before-test
```

Choose a DNS server reachable from the container; replace the placeholder
before executing. Docker embedded DNS (often 127.0.0.11) adds a forwarding
layer; use a team-approved direct server when testing glibc resolver retries.

```sh
DNS_SERVER=YOUR_REACHABLE_DNS_SERVER_IP
printf 'nameserver %s\noptions timeout:2 attempts:2\n' "$DNS_SERVER" > /etc/resolv.conf
cat /etc/resolv.conf
curl-probe --url https://example.com/ --verify-peer --stop-after-ms 1000
```

The file is written in place because Docker mounts it. Changes affect only
this container. timeout x attempts is not a whole-getaddrinfo deadline.
Healthy DNS can still return quickly. Keep URL/curl options/stop delay constant
when comparing resolver settings. Nothing in these commands blocks ARP or DNS.

Restore after testing:

```sh
cat /tmp/resolv.conf.before-test > /etc/resolv.conf
```

## Measure getaddrinfo (Client Shell)

The image includes /usr/local/lib/libresolver_trace.so, built from
resolver_trace.c. Enable it for one command only:

```sh
LD_PRELOAD=/usr/local/lib/libresolver_trace.so curl-probe --url https://example.com/ --verify-peer --stop-after-ms 1000
echo "probe_exit=$?"
```

The wrapper observes the real glibc getaddrinfo call via RTLD_NEXT. It does not
replace DNS answers, change resolver settings or configure different timeouts.
It writes resolver events to stderr; probe events remain on stdout.

- resolver_event=begin: hostname, service, Linux thread ID (tid), address family
    and monotonic_ms at resolver entry. Family 0 is AF_UNSPEC.
- resolver_event=end: same tid, monotonic_ms, duration_ms and resolver status.
    Status 0 means success; nonzero values are EAI errors, not CURLcode values.
    errno is meaningful for EAI_SYSTEM; do not treat it as the result otherwise.
- A begin without an end means no return was observed within the capture. It
    is not a measured complete duration and does not prove a permanent deadlock.

duration_ms measures getaddrinfo, including NSS/DNS processing, not only wire
DNS latency. The wrapper's timestamps use CLOCK_MONOTONIC; the probe reports
relative elapsed_ms, so their raw timestamp values have different origins.
Logging adds overhead. Compare traced and untraced runs and repeat each case.
No wrapper events may mean cache reuse, a literal IP, a different resolver
backend (such as c-ares), or a path not intercepted by LD_PRELOAD. This method
requires dynamically linked processes and is not a universal resolver tracer.

Save both streams without hiding the probe exit status:

```sh
LD_PRELOAD=/usr/local/lib/libresolver_trace.so curl-probe --url https://example.com/ --verify-peer --stop-after-ms 1000 > /results/probe.txt 2> /results/resolver.txt
echo "probe_exit=$?"
```

## Trace Resolver poll Calls (Client Shell)

```sh
strace -f -ttt -T -o /results/trace.txt -e trace=network,poll,ppoll,select,pselect6,futex env LD_PRELOAD=/usr/local/lib/libresolver_trace.so curl-probe --url https://example.com/ --verify-peer --stop-after-ms 1000
```

Find the resolver tid from the wrapper events in the strace output and identify
its DNS socket sends/receives and poll waits. -T reports duration of each syscall;
-f follows the internal resolver thread. Not every poll in the process is DNS.
strace -ttt timestamps are wall-clock based, unlike wrapper monotonic_ms;
correlate by tid, event order and durations rather than equating raw timestamps.

In another Client shell (Rancher Desktop terminal or another host exec):

```sh
tcpdump -i eth0 -nn -s0 -U -w /results/client.pcap 'arp or icmp6 or port 53'
```

Stop captures with Ctrl+C. Trace affects timing; compare untraced runs first.
Record URL, resolver settings, neighbor state, curl code, perform wall_ms,
release-to-join duration and DNS query/reply times for at least five runs.
namelookup_s is a completed curl metric, not a guaranteed outstanding resolver
duration. Zero on failure does not prove DNS was instant.

## Manual ARP Tests

No faults are automatic. The retired local-peer scripts and design.html have
been removed. Use design2.html for the current application walkthrough.

Identify the actual DNS next hop with ip route get before coordinating manual
fault injection. A direct DNS server and a server reached via a gateway have
different neighbor targets. A container gateway ARP fault may affect all its
traffic, not just DNS. Docker/WSL networking does not reproduce device Wi-Fi.
Only apply explicit team-approved fault commands inside the container and
restore afterward; never apply them to the host or production device.

## Finish

Restore resolver changes and exit the Client shell. On the host:

```sh
docker compose down
```

See design2.html for every probe argument, thread flow and timing field.