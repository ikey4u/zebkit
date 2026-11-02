# Playwright's commands: native, zebkit's endpoint and over CDP

`scripts/automation-bench.sh`, 50 runs of each (20 of `goto` and `screenshot`) after three to warm up,
headless, tests/bench/automation/index.html served locally. Playwright is playwright-core 1.64.1 in Node.js.
Medians in milliseconds; p90 in parentheses.

| Operation | zebkit native | zebkit endpoint (Playwright protocol) (155.0.8059.84) | zebkit over CDP (155.0.8059.84) | Chrome over CDP (156.0.8078.4) | native vs over CDP |
|---|---|---|---|---|---|
| goto | 13.46 (16.23) | 13.88 (17.31) | 15.77 (20.69) | 18.31 (20.86) | 1.2x faster |
| evaluate 1+1 | 0.14 (0.17) | 0.59 (0.67) | 0.65 (0.85) | 0.47 (0.58) | 4.6x faster |
| locator.textContent | 0.26 (0.36) | 0.67 (0.78) | 1.02 (1.39) | 0.94 (1.17) | 3.9x faster |
| locator.click | 4.91 (5.10) | 3.43 (3.82) | 33.30 (34.09) | 33.29 (34.04) | 6.8x faster |
| locator.fill | 0.69 (2.88) | 2.55 (2.85) | 3.73 (6.05) | 2.68 (4.65) | 5.4x faster |
| locator.press Enter | 0.42 (0.53) | 0.86 (1.27) | 3.41 (3.92) | 2.28 (2.72) | 8.1x faster |
| locator.isVisible | 0.23 (0.26) | 0.68 (1.06) | 0.76 (0.86) | 0.62 (0.72) | 3.3x faster |
| locator.count (200) | 0.27 (0.30) | 0.69 (0.88) | 0.80 (0.95) | 0.69 (1.25) | 3.0x faster |
| allTextContents (200) | 0.34 (0.36) | 1.84 (2.33) | 2.33 (3.38) | 2.04 (2.53) | 6.9x faster |
| getByRole click | 3.21 (5.83) | 3.69 (6.11) | 33.31 (34.17) | 33.27 (34.33) | 10.4x faster |
| screenshot | 41.46 (47.49) | 52.44 (54.89) | 66.71 (67.93) | 33.60 (37.30) | 1.6x faster |
