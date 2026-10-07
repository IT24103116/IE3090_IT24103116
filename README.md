# IE3090_IT24103116 - RemoteOps

Module: IE3090 Network Programming, SLIIT Faculty of Computing

Registration number: IT24103116

## Personalised values (calculated from IT24103116)

| Item | Calculation | Value |
|---|---|---|
| Agent port | 7000 + 2410 | 9410 |
| Source files | last 3 digits = 116 | agent_116.c, controller_116.c, Makefile_116 |
| SID tag | last 4 digits 3116 reversed | SID:6113 |
| Auth token | OPS- + 3116 | OPS-3116 |
| Log file | | remoteops_IT24103116.log |
| Storage path | | ./agentfiles/IT24103116/<filename> |
| ZIP archive | | IE3090_IT24103116.zip |

## Build (Linux, gcc)

    make -f Makefile_116

## Run

    ./agent_116                  # TCP 9410, logs to remoteops_IT24103116.log
    ./controller_116 [agent-ip]  # default agent-ip is 127.0.0.1

## Controller commands

    AUTH OPS-3116           must be the first command
    SYSINFO                 CPU load, memory used (MB), uptime (s)
    LISTPROC                process snapshot
    EXEC <name>             DATE, UPTIME, DISKFREE, HOSTNAME, WHOAMI only
    PUT <localfile>         upload to ./agentfiles/IT24103116/
    GET <name>              download to ./downloads/
    MONITOR START <port>    UDP stats every 2 s to this port
    MONITOR STOP
    QUIT

## Design summary

- Thread per client connection (pthreads), mutex-protected log file
- Line framing with a per-connection buffer; PUT/GET move exactly <filesize> bytes
- Uploads written to a temp file then renamed; file names validated; 50 MB limit
- Optional extension: transfer throughput (bytes/s) reported for PUT and GET
