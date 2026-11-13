# Playwright's commands: native, zebkit's endpoint and over CDP

Engine: zebkit 0.5.0 (Chromium 157.0.8103.38), profile slim, measured 2026-11-13.

`scripts/automation-bench.sh`, 50 runs of each (20 of `goto` and of each screenshot) after three to warm up,
headless, tests/bench/automation/index.html served locally. Playwright is playwright-core 1.64.1 in Node.js.
Medians in milliseconds; p90 in parentheses.

| Operation | zebkit native | zebkit endpoint (Playwright protocol) (157.0.8103.38) | zebkit over CDP (157.0.8103.38) | Chrome over CDP (156.0.8078.4) | native vs over CDP |
|---|---|---|---|---|---|
| goto | 10.67 (14.48) | 12.56 (13.71) | 11.86 (16.04) | 18.12 (21.76) | 1.1x faster |
| evaluate 1+1 | 0.27 (0.35) | 0.66 (0.85) | 0.61 (0.86) | 0.46 (0.68) | 2.3x faster |
| locator.textContent | 0.23 (0.32) | 0.74 (0.97) | 1.14 (1.39) | 0.90 (1.01) | 5.0x faster |
| locator.click | 1.93 (3.00) | 3.57 (4.52) | 33.40 (34.23) | 33.40 (33.88) | 17.3x faster |
| locator.fill | 2.11 (2.18) | 2.13 (2.63) | 3.83 (5.79) | 3.20 (8.28) | 1.8x faster |
| locator.press Enter | 0.77 (0.99) | 0.91 (1.11) | 3.24 (4.68) | 2.36 (2.63) | 4.2x faster |
| locator.isVisible | 0.29 (0.31) | 0.63 (0.73) | 0.71 (0.77) | 0.84 (1.21) | 2.4x faster |
| locator.count (200) | 0.23 (0.26) | 0.68 (0.75) | 0.76 (0.80) | 0.73 (1.05) | 3.3x faster |
| allTextContents (200) | 0.30 (0.40) | 1.86 (2.80) | 2.13 (2.44) | 1.96 (2.41) | 7.1x faster |
| getByRole click | 2.92 (3.09) | 3.67 (4.60) | 33.33 (34.64) | 33.29 (33.81) | 11.4x faster |
| screenshot | 13.41 (14.63) | 17.16 (26.88) | 50.34 (53.45) | 33.43 (35.82) | 3.8x faster |
| screenshot jpeg | 13.50 (15.04) | 13.58 (16.01) | 47.16 (50.25) | 33.16 (35.45) | 3.5x faster |
| screenshot clip | 13.60 (15.47) | 17.38 (25.12) | 50.00 (51.70) | 33.84 (44.92) | 3.7x faster |
| screenshot pixels | 9.59 (10.96) |  |  |  |  |
