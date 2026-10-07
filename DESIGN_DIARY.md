# Design Diary

## 7 Oct 2026
- Started implementation today, so all my commits fall on this date.
- Created the repo and calculated my personalised values (see README).
- Plan: agent skeleton, controller skeleton, framing and AUTH, command handlers, threads and logging, PUT/GET, UDP monitoring, testing.

## Notes added during development (7 Oct 2026)
- Developed in stages: sockets and accept loop, controller, line framing + AUTH, command handlers, threads + logging, PUT/GET, UDP monitoring.
- Chose thread-per-client: simple blocking I/O per client, shared log needs only a mutex, and 5+ clients is well within what threads handle.
- Framing: each connection keeps a buffer, so a half line or two lines in one recv() are handled. For PUT, bytes already buffered after the command line are used before calling recv() again.
- Uploads go to a temp file and are renamed on success, so a half-uploaded file is never visible and a dropped upload leaves nothing behind.
- UDP monitoring runs in its own thread per session so the TCP command loop keeps working; it sends every 2 seconds and is stopped by MONITOR STOP, QUIT or a dropped connection.
- Obstacles: large terminal pastes were cut off or garbled, so I switched to nano and smaller pastes and checked file endings with tail. I also ran the old agent binary by mistake once because make said nothing needed rebuilding.
- My VM clock was about 8 hours behind real time, so commit timestamps look earlier than the initial commit made on GitHub.
- AI tools were used occasionally as a reference for programming concepts, Git commands, troubleshooting, and understanding implementation options. The code was implemented, compiled, tested, and debugged during development, with testing used to verify the behaviour of each feature.

