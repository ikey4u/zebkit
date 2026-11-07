# Playwright's commands: native, zebkit's endpoint and over CDP

`scripts/automation-bench.sh`, 50 runs of each (20 of `goto` and of each screenshot) after three to warm up,
headless, tests/bench/automation/index.html served locally. Playwright is playwright-core 1.64.1 in Node.js.
Medians in milliseconds; p90 in parentheses.

| Operation | zebkit native | zebkit endpoint (Playwright protocol) (155.0.8059.84) | zebkit over CDP (155.0.8059.84) | Chrome over CDP (156.0.8078.4) | native vs over CDP |
|---|---|---|---|---|---|
| goto | 11.15 (12.40) | 12.98 (13.67) | 11.15 (13.75) | 18.24 (21.40) | 1.0x faster |
| evaluate 1+1 | 0.24 (0.29) | 0.65 (0.75) | 0.57 (0.69) | 0.49 (0.60) | 2.4x faster |
| locator.textContent | 0.23 (0.33) | 0.70 (0.82) | 1.12 (1.31) | 0.81 (1.08) | 4.9x faster |
| locator.click | 2.04 (3.08) | 3.47 (4.04) | 33.40 (34.05) | 33.31 (34.34) | 16.4x faster |
| locator.fill | 2.14 (2.30) | 2.01 (2.57) | 3.52 (5.02) | 2.86 (5.43) | 1.6x faster |
| locator.press Enter | 0.59 (0.93) | 0.98 (1.48) | 3.36 (5.17) | 2.64 (2.96) | 5.7x faster |
| locator.isVisible | 0.48 (0.51) | 0.86 (0.97) | 0.76 (1.20) | 0.62 (0.78) | 1.6x faster |
| locator.count (200) | 0.23 (0.27) | 0.71 (1.20) | 0.93 (1.54) | 0.64 (0.72) | 4.0x faster |
| allTextContents (200) | 0.34 (0.37) | 1.86 (2.46) | 2.29 (3.32) | 1.91 (3.29) | 6.7x faster |
| getByRole click | 3.01 (3.09) | 3.65 (4.63) | 33.39 (34.44) | 33.30 (34.15) | 11.1x faster |
| screenshot | 12.43 (14.50) | 16.71 (17.63) | 50.20 (53.87) | 33.82 (47.16) | 4.0x faster |
| screenshot jpeg | 13.88 (20.05) | 14.43 (17.01) | 49.57 (52.72) | 32.89 (39.62) | 3.6x faster |
| screenshot clip | 13.19 (17.43) | 18.80 (27.57) | 51.16 (53.69) | 33.44 (51.21) | 3.9x faster |
| screenshot pixels | 9.27 (15.89) |  |  |  |  |
