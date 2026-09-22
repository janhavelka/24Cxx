# TMP112 shutdown sequencing: TI support research note

Source: [TI E2E thread 571503](https://e2e.ti.com/support/sensors-group/sensors/f/sensors-forum/571503/tmp112-time-to-get-a-brief-moment-data-in-low-power-comsumption).

Reviewed 2026-09-22. This is a local paraphrase of indexed TI support responses, not an original HTML snapshot. Direct downloads of the canonical URL, legacy URL and answer-filtered URL returned HTTP 403. The search index provided the discussion text, including responses attributed to TI engineers Ren Schackmann and Emmy Denton. Original discussion rights remain with their owners.

TI describes shutdown as taking effect after an active conversion finishes. Its conservative procedure requests SD=1, allows up to 35 ms for that conversion, then reads the result. From settled shutdown, writing SD=1 and OS=1 requests another conversion; the sensor returns to shutdown when it completes. The conversion-rate interval already includes conversion time.

The discussion also explains that the power-on reset threshold is below the minimum supply rating and is not specified. A conversion begun during a slow supply ramp can have reduced accuracy. TI recommends waiting at least two conversion times after the supply reaches its intended operating voltage when accuracy matters during startup.

The 35 ms value reflects the older TMP112 discussed there. Current catalog and automotive conversion limits differ; consult the archived electrical-characteristics tables for the selected device.
